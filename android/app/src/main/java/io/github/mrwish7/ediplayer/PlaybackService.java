package io.github.mrwish7.ediplayer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

/**
 * Keeps playback going with the screen off or another app in front. Without a foreground
 * service Android freezes the app and cuts its network within seconds. Also: the media
 * notification and session (lock screen, headset and Bluetooth controls), the wake, Wi-Fi
 * and multicast locks, and audio focus.
 *
 * The player itself is native and runs in this process; this service follows its state,
 * which MainActivity.updateMedia() passes on, and sends the media controls' commands back.
 */
public class PlaybackService extends Service {
    private static final String TAG = "EDIplayer";

    /** What is playing, from native code (MediaInfo in src/gui/app.h) */
    static final class State {
        boolean playing, multicast;
        String station = "", title = "", artist = "", album = "";
        Bitmap art;
    }

    // Commands to the native player: MediaCommand in src/gui/app.h
    static final int CMD_PLAY = 0, CMD_STOP = 1, CMD_NEXT = 2, CMD_PREVIOUS = 3, CMD_MUTE = 4, CMD_UNMUTE = 5;

    private static final String CHANNEL = "playback";
    private static final int NOTIFICATION_ID = 1;
    private static final String ACTION_COMMAND = "command", ACTION_DISMISS = "dismiss";
    private static final String EXTRA_COMMAND = "command";

    private static State state = new State();
    private static PlaybackService instance;

    private MediaSession session;
    private NotificationManager notifications;
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;
    private AudioManager audio;
    private AudioFocusRequest focusRequest;
    private boolean foreground;   // a foreground service, with its notification
    private boolean shown;        // the notification is shown (also after stopping: it offers Play)
    private boolean holding;      // locks, audio focus and the headphone receiver held

    // headphones unplugged: stop rather than carry on through the speaker
    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            MainActivity.nativeMediaCommand(CMD_STOP);
        }
    };

    /** New state from native code (on the UI thread). Starts the service when playing begins. */
    static void update(Context context, State s) {
        if (!s.playing) {   // stopped: keep showing what was playing, offering to play it again
            s.station = state.station;
            s.title = state.title;
            s.artist = state.artist;
            s.album = state.album;
            s.art = state.art;
        }
        state = s;
        if (instance != null) {
            instance.apply();
        } else if (s.playing) {
            try {
                context.startForegroundService(new Intent(context, PlaybackService.class));
            } catch (RuntimeException e) {   // e.g. not allowed from the background
                Log.w(TAG, "cannot start the playback service", e);
            }
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(
                new NotificationChannel(CHANNEL, "Playback", NotificationManager.IMPORTANCE_LOW));

        session = new MediaSession(this, "EDIplayer");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { MainActivity.nativeMediaCommand(CMD_PLAY); }
            @Override public void onPause() { MainActivity.nativeMediaCommand(CMD_STOP); }   // live radio: pausing stops
            @Override public void onStop() { MainActivity.nativeMediaCommand(CMD_STOP); }
            @Override public void onSkipToNext() { MainActivity.nativeMediaCommand(CMD_NEXT); }
            @Override public void onSkipToPrevious() { MainActivity.nativeMediaCommand(CMD_PREVIOUS); }
        });
        session.setSessionActivity(openAppIntent());

        wakeLock = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "EDIplayer:playback");
        wakeLock.setReferenceCounted(false);
        WifiManager wifi = getSystemService(WifiManager.class);
        // Wi-Fi power saving with the screen off delays packets in bursts, and can drop multicast
        wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "EDIplayer:playback");
        wifiLock.setReferenceCounted(false);
        multicastLock = wifi.createMulticastLock("EDIplayer:multicast");
        multicastLock.setReferenceCounted(false);

        audio = getSystemService(AudioManager.class);
        focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setOnAudioFocusChangeListener(this::onAudioFocusChange)
                .build();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent != null ? intent.getAction() : null;
        if (ACTION_COMMAND.equals(action)) {
            MainActivity.nativeMediaCommand(intent.getIntExtra(EXTRA_COMMAND, -1));
        } else if (ACTION_DISMISS.equals(action)) {   // the stopped notification swiped away
            if (!state.playing) stopSelf();
            return START_NOT_STICKY;
        }
        apply();
        if (!foreground && action == null) {
            // started by startForegroundService() but already stopped again: Android still
            // requires startForeground()
            startForeground(NOTIFICATION_ID, buildNotification(state), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
        }
        // the player is native: if Android kills the process, there is nothing to restart
        return START_NOT_STICKY;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // swiping the app away from Recents stops it
        MainActivity.nativeMediaCommand(CMD_STOP);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        hold(false, false);
        if (foreground) stopForeground(STOP_FOREGROUND_REMOVE);
        notifications.cancel(NOTIFICATION_ID);
        session.release();
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /** Bring the session, notification, foreground state and locks up to date with `state` */
    private void apply() {
        final State s = state;
        MediaMetadata.Builder meta = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, s.title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, s.artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, s.album);
        if (s.art != null) meta.putBitmap(MediaMetadata.METADATA_KEY_ART, s.art);
        session.setMetadata(meta.build());
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
                        | PlaybackState.ACTION_STOP | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS)
                .setState(s.playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_STOPPED,
                        PlaybackState.PLAYBACK_POSITION_UNKNOWN, 1.0f)
                .build());

        final Notification n = buildNotification(s);
        if (s.playing) {
            session.setActive(true);
            if (!foreground) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
                foreground = true;
            } else {
                notifications.notify(NOTIFICATION_ID, n);
            }
            shown = true;
            hold(true, s.multicast);
        } else {
            hold(false, false);
            if (foreground) {
                // keep the notification, now dismissable, offering Play
                stopForeground(STOP_FOREGROUND_DETACH);
                foreground = false;
            }
            if (shown) notifications.notify(NOTIFICATION_ID, n);
        }
    }

    private void hold(boolean on, boolean multicast) {
        if (on && !holding) {
            wakeLock.acquire();
            wifiLock.acquire();
            audio.requestAudioFocus(focusRequest);
            IntentFilter noisy = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(noisyReceiver, noisy, Context.RECEIVER_NOT_EXPORTED);
            else registerReceiver(noisyReceiver, noisy);
            holding = true;
        } else if (!on && holding) {
            wakeLock.release();
            wifiLock.release();
            audio.abandonAudioFocusRequest(focusRequest);
            unregisterReceiver(noisyReceiver);
            holding = false;
        }
        // needed to receive multicast on Wi-Fi at all on many phones
        if (on && multicast) {
            if (!multicastLock.isHeld()) multicastLock.acquire();
        } else if (multicastLock.isHeld()) {
            multicastLock.release();
        }
    }

    private void onAudioFocusChange(int change) {
        switch (change) {
            case AudioManager.AUDIOFOCUS_LOSS:   // another app plays
                MainActivity.nativeMediaCommand(CMD_STOP);
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:   // e.g. a phone call: mute, but keep receiving
                MainActivity.nativeMediaCommand(CMD_MUTE);
                break;
            case AudioManager.AUDIOFOCUS_GAIN:
                MainActivity.nativeMediaCommand(CMD_UNMUTE);
                break;
            default:   // ducking (e.g. navigation prompts) is done by the system
                break;
        }
    }

    private Notification buildNotification(State s) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(s.title)
                .setContentText(s.artist)
                .setSubText(s.station)
                .setLargeIcon(s.art)
                .setContentIntent(openAppIntent())
                .setDeleteIntent(serviceIntent(ACTION_DISMISS, -1))
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setShowWhen(false)
                .setOngoing(s.playing)
                .addAction(action(R.drawable.ic_previous, "Previous", CMD_PREVIOUS))
                .addAction(s.playing ? action(R.drawable.ic_stop, "Stop", CMD_STOP)
                                     : action(R.drawable.ic_play, "Play", CMD_PLAY))
                .addAction(action(R.drawable.ic_next, "Next", CMD_NEXT))
                .setStyle(new Notification.MediaStyle()
                        .setMediaSession(session.getSessionToken())
                        .setShowActionsInCompactView(0, 1, 2));
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return b.build();
    }

    private Notification.Action action(int icon, String title, int cmd) {
        return new Notification.Action.Builder(Icon.createWithResource(this, icon), title,
                serviceIntent(ACTION_COMMAND, cmd)).build();
    }

    private PendingIntent serviceIntent(String action, int cmd) {
        Intent i = new Intent(this, PlaybackService.class).setAction(action).putExtra(EXTRA_COMMAND, cmd);
        return PendingIntent.getService(this, cmd + 1, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private PendingIntent openAppIntent() {
        return PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
    }
}
