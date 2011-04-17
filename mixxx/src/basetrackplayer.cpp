#include <QtCore>
#include <QMessageBox>

#include "basetrackplayer.h"
#include "playerinfo.h"

#include "controlobjectthreadmain.h"
#include "controlobject.h"
#include "trackinfoobject.h"
#include "engine/enginebuffer.h"
#include "engine/enginedeck.h"
#include "engine/enginemaster.h"
#include "soundsourceproxy.h"
#include "engine/cuecontrol.h"
#include "engine/clockcontrol.h"
#include "mathstuff.h"
#include "waveform/waveformrenderer.h"

BaseTrackPlayer::BaseTrackPlayer(QObject* pParent,
                                 ConfigObject<ConfigValue> *pConfig,
                                 EngineMaster* pMixingEngine,
                                 EngineChannel::ChannelOrientation defaultOrientation,
                                 QString group)
        : BasePlayer(pParent, group),
          m_pConfig(pConfig),
          m_pLoadedTrack() {

    // Need to strdup the string because EngineChannel will save the pointer,
    // but we might get deleted before the EngineChannel. TODO(XXX)
    // pSafeGroupName is leaked. It's like 5 bytes so whatever.
    const char* pSafeGroupName = strdup(getGroup().toAscii().constData());

    EngineDeck* pChannel = new EngineDeck(pSafeGroupName,
                                          pConfig, defaultOrientation);
    EngineBuffer* pEngineBuffer = pChannel->getEngineBuffer();
    pMixingEngine->addChannel(pChannel);

    ClockControl* pClockControl = new ClockControl(pSafeGroupName, pConfig);
    pEngineBuffer->addControl(pClockControl);

    CueControl* pCueControl = new CueControl(pSafeGroupName, pConfig);
    connect(this, SIGNAL(newTrackLoaded(TrackPointer)),
            pCueControl, SLOT(loadTrack(TrackPointer)));
    connect(this, SIGNAL(unloadingTrack(TrackPointer)),
            pCueControl, SLOT(unloadTrack(TrackPointer)));
    pEngineBuffer->addControl(pCueControl);

    // Connect our signals and slots with the EngineBuffer's signals and
    // slots. This will let us know when the reader is done loading a track, and
    // let us request that the reader load a track.
    connect(this, SIGNAL(loadTrack(TrackPointer)),
            pEngineBuffer, SLOT(slotLoadTrack(TrackPointer)));
    connect(pEngineBuffer, SIGNAL(trackLoaded(TrackPointer)),
            this, SLOT(slotFinishLoading(TrackPointer)));
    connect(pEngineBuffer, SIGNAL(trackLoadFailed(TrackPointer, QString)),
            this, SLOT(slotLoadFailed(TrackPointer, QString)));
    connect(pEngineBuffer, SIGNAL(trackUnloaded(TrackPointer)),
            this, SLOT(slotUnloadTrack(TrackPointer)));

    //Get cue point control object
    m_pCuePoint = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(),"cue_point")));
    // Get loop point control objects
    m_pLoopInPoint = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(),"loop_start_position")));
    m_pLoopOutPoint = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(),"loop_end_position")));
    //Playback position within the currently loaded track (in this player).
    m_pPlayPosition = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(), "playposition")));

    // Duration of the current song, we create this one because nothing else does.
    m_pDuration = new ControlObject(ConfigKey(getGroup(), "duration"));

    //BPM of the current song
    m_pBPM = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(), "file_bpm")));

    m_pReplayGain = new ControlObjectThreadMain(
        ControlObject::getControl(ConfigKey(getGroup(), "replaygain")));

    // Create WaveformRenderer last, because it relies on controls created above
    // (e.g. EngineBuffer)

    m_pWaveformRenderer = new WaveformRenderer(pSafeGroupName);
    connect(this, SIGNAL(newTrackLoaded(TrackPointer)),
            m_pWaveformRenderer, SLOT(slotNewTrack(TrackPointer)));
    connect(this, SIGNAL(unloadingTrack(TrackPointer)),
            m_pWaveformRenderer, SLOT(slotUnloadTrack(TrackPointer)));
}

BaseTrackPlayer::~BaseTrackPlayer()
{
    if (m_pLoadedTrack) {
    	m_pLoadedTrack->setLoaded(false);
        emit(unloadingTrack(m_pLoadedTrack));
        m_pLoadedTrack.clear();
    }

    delete m_pCuePoint;
    delete m_pLoopInPoint;
    delete m_pLoopOutPoint;
    delete m_pPlayPosition;
    delete m_pBPM;
    delete m_pReplayGain;
    delete m_pWaveformRenderer;
    delete m_pDuration;
}

void BaseTrackPlayer::slotLoadTrack(TrackPointer track, bool bStartFromEndPos)
{
    //Disconnect the old track's signals.
    if (m_pLoadedTrack) {
        // Save the loops that are currently set in a loop cue. If no loop cue is
        // currently on the track, then create a new one.
        int loopStart = m_pLoopInPoint->get();
        int loopEnd = m_pLoopOutPoint->get();
        if (loopStart != -1 && loopEnd != -1 &&
            even(loopStart) && even(loopEnd) && loopStart <= loopEnd) {
            Cue* pLoopCue = NULL;
            QList<Cue*> cuePoints = m_pLoadedTrack->getCuePoints();
            QListIterator<Cue*> it(cuePoints);
            while (it.hasNext()) {
                Cue* pCue = it.next();
                if (pCue->getType() == Cue::LOOP) {
                    pLoopCue = pCue;
                }
            }
            if (!pLoopCue) {
                pLoopCue = m_pLoadedTrack->addCue();
                pLoopCue->setType(Cue::LOOP);
            }
            pLoopCue->setPosition(loopStart);
            pLoopCue->setLength(loopEnd - loopStart);
        }

        // WARNING: Never. Ever. call bare disconnect() on an object. Mixxx
        // relies on signals and slots to get tons of things done. Don't
        // randomly disconnect things.
        // m_pLoadedTrack->disconnect();
        disconnect(m_pLoadedTrack.data(), 0, m_pBPM, 0);
        disconnect(m_pLoadedTrack.data(), 0, m_pReplayGain, 0);

        // Causes the track's data to be saved back to the library database.
        m_pLoadedTrack->setLoaded(false);
        emit(unloadingTrack(m_pLoadedTrack));
    }

    m_pLoadedTrack = track;

    // Listen for updates to the file's BPM
    connect(m_pLoadedTrack.data(), SIGNAL(bpmUpdated(double)),
            m_pBPM, SLOT(slotSet(double)));

    // Listen for updates to the file's Replay Gain
    connect(m_pLoadedTrack.data(), SIGNAL(ReplayGainUpdated(double)),
            m_pReplayGain, SLOT(slotSet(double)));

    //Request a new track from the reader
    emit(loadTrack(track));
}

void BaseTrackPlayer::slotLoadFailed(TrackPointer track, QString reason) {
	m_pLoadedTrack->setLoaded(false);
    qDebug() << "Failed to load track" << track->getLocation() << reason;
    // Alert user.
    QMessageBox::warning(NULL, tr("Couldn't load track."), reason);
}

void BaseTrackPlayer::slotUnloadTrack(TrackPointer) {
    if (m_pLoadedTrack) {
        // WARNING: Never. Ever. call bare disconnect() on an object. Mixxx
        // relies on signals and slots to get tons of things done. Don't
        // randomly disconnect things.
        // m_pLoadedTrack->disconnect();
        disconnect(m_pLoadedTrack.data(), 0, m_pBPM, 0);
        disconnect(m_pLoadedTrack.data(), 0, m_pReplayGain, 0);

        // Causes the track's data to be saved back to the library database and
        // for all the widgets to unload the track and blank themselves.
        m_pLoadedTrack->setLoaded(false);
        emit(unloadingTrack(m_pLoadedTrack));
    }
    m_pDuration->set(0);
    m_pBPM->slotSet(0);
    m_pReplayGain->slotSet(0);
    m_pLoopInPoint->slotSet(-1);
    m_pLoopOutPoint->slotSet(-1);
    m_pLoadedTrack.clear();

    // Update the PlayerInfo class that is used in EngineShoutcast to replace
    // the metadata of a stream
    PlayerInfo::Instance().setTrackInfo(getGroup(), m_pLoadedTrack);
}

void BaseTrackPlayer::slotFinishLoading(TrackPointer pTrackInfoObject)
{
    // Read the tags if required
    if(!m_pLoadedTrack->getHeaderParsed())
        SoundSourceProxy::ParseHeader(m_pLoadedTrack.data());

    m_pLoadedTrack->incTimesPlayed();

    // Generate waveform summary
    //TODO: Consider reworking this visual resample stuff... need to ask rryan about this -- Albert.
    // TODO(rryan) : fix this crap -- the waveform renderers should be owned by
    // Player so they can just set this directly or something.
    ControlObjectThreadMain* pVisualResampleCO = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey(getGroup(),"VisualResample")));
    m_pLoadedTrack->setVisualResampleRate(pVisualResampleCO->get());
    delete pVisualResampleCO;

    //Update the BPM and duration values that are stored in ControlObjects
    m_pDuration->set(m_pLoadedTrack->getDuration());
    m_pBPM->slotSet(m_pLoadedTrack->getBpm());
    m_pReplayGain->slotSet(m_pLoadedTrack->getReplayGain());

    // Update the PlayerInfo class that is used in EngineShoutcast to replace
    // the metadata of a stream
    PlayerInfo::Instance().setTrackInfo(getGroup(), m_pLoadedTrack);

    // Reset the loop points.
    m_pLoopInPoint->slotSet(-1);
    m_pLoopOutPoint->slotSet(-1);

    const QList<Cue*> trackCues = pTrackInfoObject->getCuePoints();
    QListIterator<Cue*> it(trackCues);
    while (it.hasNext()) {
        Cue* pCue = it.next();
        if (pCue->getType() == Cue::LOOP) {
            int loopStart = pCue->getPosition();
            int loopEnd = loopStart + pCue->getLength();
            if (loopStart != -1 && loopEnd != -1 && even(loopStart) && even(loopEnd)) {
                m_pLoopInPoint->slotSet(loopStart);
                m_pLoopOutPoint->slotSet(loopEnd);
                break;
            }
        }
    }

    emit(newTrackLoaded(m_pLoadedTrack));
}

WaveformRenderer* BaseTrackPlayer::getWaveformRenderer() const {
    return m_pWaveformRenderer;
}

TrackPointer BaseTrackPlayer::getLoadedTrack() const {
    return m_pLoadedTrack;
}
