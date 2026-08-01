// SPDX-License-Identifier: LGPL-2.1-or-later
package org.qtav.core.player;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@SuppressWarnings("deprecation")
public final class QtAVPlayerActivity extends Activity
        implements SurfaceHolder.Callback {
    private static final int OPEN_DOCUMENT_REQUEST = 1001;
    private static final int SEEK_SCALE = 10_000;

    static {
        System.loadLibrary("qtav_android_player");
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService mediaProbeExecutor =
            Executors.newSingleThreadExecutor();

    private long nativeHandle;
    private SurfaceView surfaceView;
    private SeekBar seekBar;
    private TextView currentTimeView;
    private TextView durationView;
    private TextView statusView;
    private Button playPauseButton;
    private Switch vulkanSwitch;
    private Switch hdrSwitch;
    private Switch zeroCopySwitch;
    private Switch hardwareSwitch;
    private Switch debugSwitch;
    private boolean userSeeking;
    private long knownDuration;
    private volatile String temporaryStatus;
    private volatile boolean destroyed;
    private int openGeneration;
    private int lastRequestedFrameRateMilliHertz = -1;
    private int preferredDisplayModeId;
    private float preferredDisplayRefreshRate;

    private final Runnable pollPlayback = new Runnable() {
        @Override
        public void run() {
            if (nativeHandle == 0) {
                return;
            }
            long duration = Math.max(0, nativeGetDuration(nativeHandle));
            long position = Math.max(0, nativeGetPosition(nativeHandle));
            if (duration > 0) {
                knownDuration = duration;
            }
            if (!userSeeking) {
                currentTimeView.setText(formatTime(position));
                durationView.setText(formatTime(knownDuration));
                int progress = knownDuration > 0
                        ? (int) Math.min(
                                SEEK_SCALE,
                                position * SEEK_SCALE / knownDuration)
                        : 0;
                seekBar.setProgress(progress);
                seekBar.setEnabled(knownDuration > 0);
            }
            playPauseButton.setText(
                    nativeIsPlaying(nativeHandle) ? "Pause" : "Play");
            int requestedFrameRate =
                    nativeGetRequestedFrameRate(nativeHandle);
            if (requestedFrameRate
                    != lastRequestedFrameRateMilliHertz) {
                updatePreferredDisplayMode(requestedFrameRate);
            }
            String override = temporaryStatus;
            String nextStatus = override != null
                    ? override
                    : nativeGetStatus(nativeHandle);
            if (preferredDisplayRefreshRate > 0.0f) {
                nextStatus += String.format(
                        Locale.US,
                        " · display target %.3gfps",
                        preferredDisplayRefreshRate);
            }
            if (!nextStatus.contentEquals(statusView.getText())) {
                statusView.setText(nextStatus);
            }
            mainHandler.postDelayed(this, 250);
        }
    };

    private final Runnable applyOptions = new Runnable() {
        @Override
        public void run() {
            if (nativeHandle == 0) {
                return;
            }
            nativeSetOptions(
                    nativeHandle,
                    vulkanSwitch.isChecked(),
                    hdrSwitch.isChecked(),
                    zeroCopySwitch.isChecked(),
                    hardwareSwitch.isChecked(),
                    debugSwitch.isChecked());
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeHandle = nativeCreate(createSystemCaBundle());
        buildUserInterface();
        surfaceView.getHolder().addCallback(this);
        mainHandler.post(pollPlayback);
    }

    private String createSystemCaBundle() {
        File certificateDirectory =
                new File("/system/etc/security/cacerts");
        File[] certificates = certificateDirectory.listFiles(
                file -> file.isFile());
        if (certificates == null || certificates.length == 0) {
            return "";
        }
        Arrays.sort(certificates, Comparator.comparing(File::getName));

        File bundle = new File(getFilesDir(), "android-system-cacerts.pem");
        byte[] buffer = new byte[16 * 1024];
        try (FileOutputStream output = new FileOutputStream(bundle, false)) {
            for (File certificate : certificates) {
                try (FileInputStream input =
                             new FileInputStream(certificate)) {
                    int count;
                    while ((count = input.read(buffer)) >= 0) {
                        output.write(buffer, 0, count);
                    }
                }
                output.write('\n');
            }
            output.getFD().sync();
            return bundle.getAbsolutePath();
        } catch (IOException | SecurityException exception) {
            return "";
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nativeHandle != 0 && surfaceView != null) {
            Surface surface = surfaceView.getHolder().getSurface();
            if (surface != null && surface.isValid()) {
                nativeSetSurface(nativeHandle, surface);
            }
        }
    }

    @Override
    protected void onPause() {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, null);
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        mainHandler.removeCallbacksAndMessages(null);
        mediaProbeExecutor.shutdownNow();
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
        super.onDestroy();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, holder.getSurface());
        }
    }

    @Override
    public void surfaceChanged(
            SurfaceHolder holder,
            int format,
            int width,
            int height) {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, holder.getSurface());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, null);
        }
    }

    private void buildUserInterface() {
        final int padding = dp(8);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(
                    insets.getSystemWindowInsetLeft(),
                    insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });

        FrameLayout playerArea = new FrameLayout(this);
        playerArea.setBackgroundColor(Color.BLACK);
        root.addView(
                playerArea,
                new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        0,
                        1.0f));

        surfaceView = new SurfaceView(this);
        // The Surface and status are separate composition layers. Updating
        // status text cannot resize the video area or recreate its Surface.
        surfaceView.setZOrderOnTop(false);
        playerArea.addView(
                surfaceView,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(12.0f);
        statusView.setMaxLines(6);
        statusView.setEllipsize(android.text.TextUtils.TruncateAt.END);
        statusView.setGravity(Gravity.TOP | Gravity.START);
        statusView.setPadding(dp(6), dp(4), dp(6), dp(4));
        statusView.setBackgroundColor(Color.argb(144, 0, 0, 0));
        statusView.setClickable(false);
        statusView.setFocusable(false);
        FrameLayout.LayoutParams statusLayout =
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        dp(132),
                        Gravity.TOP | Gravity.START);
        statusLayout.setMargins(padding, padding, padding, 0);
        playerArea.addView(statusView, statusLayout);

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        controls.setPadding(padding, padding, padding, padding);
        controls.setBackgroundColor(Color.rgb(32, 32, 32));
        root.addView(
                controls,
                new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout timeline = new LinearLayout(this);
        timeline.setOrientation(LinearLayout.HORIZONTAL);
        timeline.setGravity(Gravity.CENTER_VERTICAL);
        currentTimeView = timeText("00:00");
        durationView = timeText("00:00");
        seekBar = new SeekBar(this);
        seekBar.setMax(SEEK_SCALE);
        seekBar.setEnabled(false);
        timeline.addView(currentTimeView);
        timeline.addView(
                seekBar,
                new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        timeline.addView(durationView);
        controls.addView(timeline);

        seekBar.setOnSeekBarChangeListener(
                new SeekBar.OnSeekBarChangeListener() {
                    @Override
                    public void onProgressChanged(
                            SeekBar bar,
                            int progress,
                            boolean fromUser) {
                        if (fromUser && knownDuration > 0) {
                            long preview =
                                    knownDuration * progress / SEEK_SCALE;
                            currentTimeView.setText(formatTime(preview));
                        }
                    }

                    @Override
                    public void onStartTrackingTouch(SeekBar bar) {
                        userSeeking = true;
                    }

                    @Override
                    public void onStopTrackingTouch(SeekBar bar) {
                        userSeeking = false;
                        if (nativeHandle != 0 && knownDuration > 0) {
                            nativeSeek(
                                    nativeHandle,
                                    knownDuration
                                            * bar.getProgress()
                                            / SEEK_SCALE);
                        }
                    }
                });

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        Button openLocalButton = actionButton("Open local");
        Button openRemoteButton = actionButton("Open URL");
        playPauseButton = actionButton("Play");
        Button stopButton = actionButton("Stop");
        addWeighted(buttons, openLocalButton);
        addWeighted(buttons, openRemoteButton);
        addWeighted(buttons, playPauseButton);
        addWeighted(buttons, stopButton);
        controls.addView(buttons);

        openLocalButton.setOnClickListener(view -> openLocalMedia());
        openRemoteButton.setOnClickListener(view -> showRemoteDialog());
        playPauseButton.setOnClickListener(view -> {
            if (nativeHandle != 0) {
                nativeTogglePlayback(nativeHandle);
            }
        });
        stopButton.setOnClickListener(view -> {
            if (nativeHandle != 0) {
                nativeStop(nativeHandle);
            }
        });

        LinearLayout renderOptions = new LinearLayout(this);
        renderOptions.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout decodeOptions = new LinearLayout(this);
        decodeOptions.setOrientation(LinearLayout.HORIZONTAL);
        vulkanSwitch = optionSwitch("Vulkan", true);
        hdrSwitch = optionSwitch("HDR", true);
        // Direct Surface is the smooth playback default. The private
        // AImageReader/SurfaceTexture paths remain an explicit interop test.
        zeroCopySwitch = optionSwitch("ZeroCopy", false);
        hardwareSwitch = optionSwitch("Hardware decode", true);
        debugSwitch = optionSwitch("Debug layer", false);
        addWeighted(renderOptions, vulkanSwitch);
        addWeighted(renderOptions, hdrSwitch);
        addWeighted(renderOptions, debugSwitch);
        addWeighted(decodeOptions, zeroCopySwitch);
        addWeighted(decodeOptions, hardwareSwitch);
        controls.addView(renderOptions);
        controls.addView(decodeOptions);

        View.OnClickListener optionListener = view -> {
            updateOptionAvailability();
            mainHandler.removeCallbacks(applyOptions);
            mainHandler.postDelayed(applyOptions, 250);
        };
        vulkanSwitch.setOnClickListener(optionListener);
        hdrSwitch.setOnClickListener(optionListener);
        zeroCopySwitch.setOnClickListener(optionListener);
        hardwareSwitch.setOnClickListener(optionListener);
        debugSwitch.setOnClickListener(optionListener);
        updateOptionAvailability();

        setContentView(root);
    }

    private void updateOptionAvailability() {
        boolean appRendererActive =
                !hardwareSwitch.isChecked() || zeroCopySwitch.isChecked();
        vulkanSwitch.setEnabled(appRendererActive);
        hdrSwitch.setEnabled(appRendererActive);
        debugSwitch.setEnabled(
                appRendererActive && vulkanSwitch.isChecked());
    }

    private void openLocalMedia() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(
                Intent.EXTRA_MIME_TYPES,
                new String[] {"video/*", "audio/*", "application/ogg"});
        startActivityForResult(intent, OPEN_DOCUMENT_REQUEST);
    }

    @Override
    protected void onActivityResult(
            int requestCode,
            int resultCode,
            Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != OPEN_DOCUMENT_REQUEST
                || resultCode != RESULT_OK
                || data == null
                || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        int takeFlags = data.getFlags()
                & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(
                    uri,
                    takeFlags);
        } catch (RuntimeException ignored) {
            // Some document providers grant only a one-shot descriptor.
        }
        openDocumentUri(uri);
    }

    private void openDocumentUri(Uri uri) {
        ++openGeneration;
        ParcelFileDescriptor descriptor = null;
        try {
            ContentResolver resolver = getContentResolver();
            descriptor = resolver.openFileDescriptor(uri, "r");
            if (descriptor == null) {
                throw new IllegalStateException(
                        "The document provider returned no descriptor");
            }
            MediaSize size = probeMedia(descriptor);
            String label = displayName(uri);
            int detachedDescriptor = descriptor.detachFd();
            descriptor.close();
            descriptor = null;
            knownDuration = 0;
            nativeOpenMedia(
                    nativeHandle,
                    label,
                    "",
                    detachedDescriptor,
                    size.width,
                    size.height);
        } catch (Exception exception) {
            showError("Could not open the local file", exception);
        } finally {
            if (descriptor != null) {
                try {
                    descriptor.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private void showRemoteDialog() {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setText("https://");
        input.setSelection(input.length());
        int horizontal = dp(20);
        LinearLayout container = new LinearLayout(this);
        container.setPadding(horizontal, 0, horizontal, 0);
        container.addView(
                input,
                new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT));
        new AlertDialog.Builder(this)
                .setTitle("Open remote media file")
                .setMessage(
                        "The URL is opened directly by QtAVCore/FFmpeg. "
                        + "HTTPS uses the OpenSSL build packaged in this app.")
                .setView(container)
                .setNegativeButton("Cancel", null)
                .setPositiveButton(
                        "Open",
                        (dialog, which) ->
                                openRemoteUrl(
                                        input.getText().toString().trim()))
                .show();
    }

    private void openRemoteUrl(String address) {
        if (!address.startsWith("http://")
                && !address.startsWith("https://")) {
            Toast.makeText(
                    this,
                    "Enter an http:// or https:// URL",
                    Toast.LENGTH_LONG).show();
            return;
        }
        final int generation = ++openGeneration;
        temporaryStatus =
                "Probing remote media through FFmpeg/OpenSSL…";
        mediaProbeExecutor.execute(() -> {
            try {
                long packedSize = nativeProbeVideoSize(address);
                int width = (int) (packedSize >>> 32);
                int height = (int) packedSize;
                mainHandler.post(() -> {
                    if (destroyed
                            || nativeHandle == 0
                            || generation != openGeneration) {
                        return;
                    }
                    temporaryStatus = null;
                    knownDuration = 0;
                    nativeOpenMedia(
                            nativeHandle,
                            address,
                            address,
                            -1,
                            width,
                            height);
                    if (width <= 0 || height <= 0) {
                        Toast.makeText(
                                this,
                                "FFmpeg could not report video dimensions; "
                                + "hardware ZeroCopy may need to be disabled",
                                Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Exception exception) {
                Exception reported = exception;
                mainHandler.post(() -> {
                    if (destroyed || generation != openGeneration) {
                        return;
                    }
                    temporaryStatus = null;
                    showError("Remote FFmpeg probe failed", reported);
                });
            }
        });
    }

    private MediaSize probeMedia(ParcelFileDescriptor descriptor) {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            retriever.setDataSource(descriptor.getFileDescriptor());
            return probeMedia(retriever);
        } catch (RuntimeException ignored) {
            return new MediaSize(0, 0);
        } finally {
            try {
                retriever.release();
            } catch (Exception ignored) {
            }
        }
    }

    private MediaSize probeMedia(MediaMetadataRetriever retriever) {
        int width = parsePositive(
                retriever.extractMetadata(
                        MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
        int height = parsePositive(
                retriever.extractMetadata(
                        MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
        return new MediaSize(width, height);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
                uri,
                new String[] {OpenableColumns.DISPLAY_NAME},
                null,
                null,
                null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(
                        OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String name = cursor.getString(column);
                    if (name != null && !name.isEmpty()) {
                        return name;
                    }
                }
            }
        } catch (RuntimeException ignored) {
        }
        return uri.toString();
    }

    private void showError(String title, Exception exception) {
        String detail = exception.getMessage();
        if (detail == null || detail.isEmpty()) {
            detail = exception.getClass().getSimpleName();
        }
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(detail)
                .setPositiveButton("OK", null)
                .show();
    }

    private TextView timeText(String value) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextColor(Color.WHITE);
        view.setTextSize(12.0f);
        view.setGravity(Gravity.CENTER);
        view.setMinWidth(dp(48));
        return view;
    }

    private Button actionButton(String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setMinWidth(0);
        button.setPadding(dp(2), 0, dp(2), 0);
        return button;
    }

    private Switch optionSwitch(String label, boolean checked) {
        Switch option = new Switch(this);
        option.setText(label);
        option.setTextColor(Color.WHITE);
        option.setChecked(checked);
        option.setPadding(dp(6), 0, dp(6), 0);
        return option;
    }

    private void addWeighted(LinearLayout row, View view) {
        row.addView(
                view,
                new LinearLayout.LayoutParams(0, dp(48), 1.0f));
    }

    private int dp(int value) {
        return Math.round(
                value * getResources().getDisplayMetrics().density);
    }

    private void updatePreferredDisplayMode(
            int contentRateMilliHertz) {
        lastRequestedFrameRateMilliHertz = contentRateMilliHertz;
        int nextModeId = 0;
        float nextRefreshRate = 0.0f;
        if (contentRateMilliHertz > 0) {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            float contentRate = contentRateMilliHertz / 1000.0f;
            Display.Mode exactMultiple = null;
            Display.Mode fastest = null;
            for (Display.Mode candidate : display.getSupportedModes()) {
                if (candidate.getPhysicalWidth()
                                != current.getPhysicalWidth()
                        || candidate.getPhysicalHeight()
                                != current.getPhysicalHeight()) {
                    continue;
                }
                if (fastest == null
                        || candidate.getRefreshRate()
                                > fastest.getRefreshRate()) {
                    fastest = candidate;
                }
                float ratio = candidate.getRefreshRate() / contentRate;
                int cadence = Math.max(1, Math.round(ratio));
                if (Math.abs(ratio - cadence) <= 0.01f
                        && (exactMultiple == null
                            || candidate.getRefreshRate()
                                    < exactMultiple.getRefreshRate())) {
                    exactMultiple = candidate;
                }
            }
            Display.Mode selected = exactMultiple != null
                    ? exactMultiple
                    : fastest;
            if (selected != null) {
                nextModeId = selected.getModeId();
                nextRefreshRate = selected.getRefreshRate();
            }
        }
        if (preferredDisplayModeId == nextModeId) {
            preferredDisplayRefreshRate = nextRefreshRate;
            return;
        }
        WindowManager.LayoutParams attributes =
                getWindow().getAttributes();
        attributes.preferredDisplayModeId = nextModeId;
        getWindow().setAttributes(attributes);
        preferredDisplayModeId = nextModeId;
        preferredDisplayRefreshRate = nextRefreshRate;
    }

    private static int parsePositive(String value) {
        if (value == null) {
            return 0;
        }
        try {
            return Math.max(0, Integer.parseInt(value));
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }

    private static String formatTime(long milliseconds) {
        long seconds = Math.max(0, milliseconds / 1000);
        long hours = seconds / 3600;
        long minutes = (seconds % 3600) / 60;
        long remainder = seconds % 60;
        return hours > 0
                ? String.format(
                        Locale.US,
                        "%d:%02d:%02d",
                        hours,
                        minutes,
                        remainder)
                : String.format(
                        Locale.US,
                        "%02d:%02d",
                        minutes,
                        remainder);
    }

    private static final class MediaSize {
        final int width;
        final int height;

        MediaSize(int width, int height) {
            this.width = width;
            this.height = height;
        }
    }

    private native long nativeCreate(String caBundlePath);
    private native void nativeDestroy(long handle);
    private native void nativeSetSurface(long handle, Surface surface);
    private native void nativeOpenMedia(
            long handle,
            String label,
            String path,
            int descriptor,
            int videoWidth,
            int videoHeight);
    private native long nativeProbeVideoSize(String address);
    private native void nativeSetOptions(
            long handle,
            boolean vulkan,
            boolean hdr,
            boolean zeroCopy,
            boolean hardwareDecode,
            boolean debugLayer);
    private native void nativeTogglePlayback(long handle);
    private native void nativeStop(long handle);
    private native void nativeSeek(long handle, long position);
    private native long nativeGetPosition(long handle);
    private native long nativeGetDuration(long handle);
    private native boolean nativeIsPlaying(long handle);
    private native int nativeGetRequestedFrameRate(long handle);
    private native String nativeGetStatus(long handle);
}
