// SPDX-License-Identifier: LGPL-2.1-or-later
package org.qtav.core.player;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
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
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
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

@SuppressWarnings("deprecation")
public final class QtAVPlayerActivity extends Activity
        implements SurfaceHolder.Callback {
    private static final int OPEN_DOCUMENT_REQUEST = 1001;
    private static final int SEEK_SCALE = 10_000;
    private static final long FULLSCREEN_CONTROLS_TIMEOUT_MILLIS = 5_000;

    static {
        System.loadLibrary("qtav_android_player");
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private long nativeHandle;
    private LinearLayout rootLayout;
    private FrameLayout playerArea;
    private LinearLayout controlsPanel;
    private SurfaceView surfaceView;
    private SeekBar seekBar;
    private TextView currentTimeView;
    private TextView durationView;
    private TextView statusView;
    private Button playPauseButton;
    private Button fullscreenButton;
    private Switch vulkanSwitch;
    private Switch hdrSwitch;
    private Switch zeroCopySwitch;
    private Switch hardwareSwitch;
    private Switch debugSwitch;
    private boolean userSeeking;
    private long knownDuration;
    private int lastRequestedFrameRateMilliHertz = -1;
    private int preferredDisplayModeId;
    private float preferredDisplayRefreshRate;
    private int videoWidth;
    private int videoHeight;
    private int publishedDisplayRotation = -1;
    private boolean fullscreenMode;
    private boolean consumeFullscreenRevealGesture;

    private final Runnable pollPlayback = new Runnable() {
        @Override
        public void run() {
            if (nativeHandle == 0) {
                return;
            }
            int currentRotation = currentDisplayRotation();
            if (currentRotation != publishedDisplayRotation
                    && surfaceView != null) {
                Surface currentSurface =
                        surfaceView.getHolder().getSurface();
                if (currentSurface != null && currentSurface.isValid()) {
                    publishSurface(currentSurface);
                }
            }
            if (nativeApplyPendingVideoFallback(nativeHandle)) {
                zeroCopySwitch.setChecked(false);
                updateOptionAvailability();
                applyHdrPreference();
                applyVideoSurfaceLayout();
            }
            long packedVideoSize = nativeGetVideoSize(nativeHandle);
            int nextVideoWidth = (int) (packedVideoSize >>> 32);
            int nextVideoHeight = (int) packedVideoSize;
            if (videoWidth != nextVideoWidth
                    || videoHeight != nextVideoHeight) {
                videoWidth = nextVideoWidth;
                videoHeight = nextVideoHeight;
                applyVideoSurfaceLayout();
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
            String playbackStatus = nativeGetStatus(nativeHandle);
            if (preferredDisplayRefreshRate > 0.0f) {
                playbackStatus += String.format(
                        Locale.US,
                        " · display target %.3gfps",
                        preferredDisplayRefreshRate);
            }
            String nextStatus = buildOutputDiagnostics()
                    + "\n" + playbackStatus;
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
                    hardwareSwitch.isChecked());
        }
    };

    private final Runnable hideFullscreenControls = () -> {
        if (fullscreenMode && !userSeeking && controlsPanel != null) {
            controlsPanel.setVisibility(View.GONE);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeHandle = nativeCreate(createSystemCaBundle());
        buildUserInterface();
        if (getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE) {
            setFullscreenMode(true);
        }
        surfaceView.getHolder().addCallback(this);
        mainHandler.post(pollPlayback);
    }

    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (configuration.orientation
                == Configuration.ORIENTATION_LANDSCAPE) {
            setFullscreenMode(true);
        } else if (configuration.orientation
                   == Configuration.ORIENTATION_PORTRAIT) {
            setFullscreenMode(false);
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (fullscreenMode && controlsPanel != null) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN
                    && controlsPanel.getVisibility() != View.VISIBLE) {
                showFullscreenControls();
                consumeFullscreenRevealGesture = true;
                return true;
            }
            if (consumeFullscreenRevealGesture) {
                if (action == MotionEvent.ACTION_UP
                        || action == MotionEvent.ACTION_CANCEL) {
                    consumeFullscreenRevealGesture = false;
                }
                return true;
            }
            if (action == MotionEvent.ACTION_DOWN) {
                showFullscreenControls();
            }
        }
        return super.dispatchTouchEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && fullscreenMode) {
            applyFullscreenSystemUi();
        }
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
        if (fullscreenMode) {
            applyFullscreenSystemUi();
            showFullscreenControls();
        }
        if (nativeHandle != 0 && surfaceView != null) {
            Surface surface = surfaceView.getHolder().getSurface();
            if (surface != null && surface.isValid()) {
                publishSurface(surface);
            }
        }
    }

    @Override
    protected void onPause() {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, null, currentDisplayRotation());
            publishedDisplayRotation = -1;
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacksAndMessages(null);
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
        super.onDestroy();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (nativeHandle != 0) {
            publishSurface(holder.getSurface());
        }
    }

    @Override
    public void surfaceChanged(
            SurfaceHolder holder,
            int format,
            int width,
            int height) {
        if (nativeHandle != 0) {
            publishSurface(holder.getSurface());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (nativeHandle != 0) {
            nativeSetSurface(nativeHandle, null, currentDisplayRotation());
            publishedDisplayRotation = -1;
        }
    }

    private void buildUserInterface() {
        final int padding = dp(8);
        rootLayout = new LinearLayout(this);
        rootLayout.setOrientation(LinearLayout.VERTICAL);
        rootLayout.setBackgroundColor(Color.BLACK);
        rootLayout.setOnApplyWindowInsetsListener((view, insets) -> {
            if (fullscreenMode) {
                view.setPadding(0, 0, 0, 0);
            } else {
                view.setPadding(
                        insets.getSystemWindowInsetLeft(),
                        insets.getSystemWindowInsetTop(),
                        insets.getSystemWindowInsetRight(),
                        insets.getSystemWindowInsetBottom());
            }
            return insets;
        });

        playerArea = new FrameLayout(this);
        playerArea.setBackgroundColor(Color.BLACK);
        rootLayout.addView(
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
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        Gravity.CENTER));
        playerArea.addOnLayoutChangeListener(
                (view, left, top, right, bottom,
                        oldLeft, oldTop, oldRight, oldBottom) ->
                        applyVideoSurfaceLayout());

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(12.0f);
        statusView.setMaxLines(8);
        statusView.setEllipsize(android.text.TextUtils.TruncateAt.END);
        statusView.setGravity(Gravity.TOP | Gravity.START);
        statusView.setPadding(dp(6), dp(4), dp(6), dp(4));
        statusView.setBackgroundColor(Color.argb(144, 0, 0, 0));
        statusView.setClickable(false);
        statusView.setFocusable(false);
        FrameLayout.LayoutParams statusLayout =
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        dp(180),
                        Gravity.TOP | Gravity.START);
        statusLayout.setMargins(padding, padding, padding, 0);
        playerArea.addView(statusView, statusLayout);

        controlsPanel = new LinearLayout(this);
        controlsPanel.setOrientation(LinearLayout.VERTICAL);
        controlsPanel.setPadding(padding, padding, padding, padding);
        controlsPanel.setBackgroundColor(Color.rgb(32, 32, 32));
        rootLayout.addView(
                controlsPanel,
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
        controlsPanel.addView(timeline);

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
                        mainHandler.removeCallbacks(
                                hideFullscreenControls);
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
                        scheduleFullscreenControlsHide();
                    }
                });

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        Button openLocalButton = actionButton("Open local");
        Button openRemoteButton = actionButton("Open URL");
        playPauseButton = actionButton("Play");
        Button stopButton = actionButton("Stop");
        fullscreenButton = actionButton("Full screen");
        addWeighted(buttons, openLocalButton);
        addWeighted(buttons, openRemoteButton);
        addWeighted(buttons, playPauseButton);
        addWeighted(buttons, stopButton);
        addWeighted(buttons, fullscreenButton);
        controlsPanel.addView(buttons);

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
        fullscreenButton.setOnClickListener(
                view -> setFullscreenMode(!fullscreenMode));

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
        debugSwitch = optionSwitch("Debug", true);
        addWeighted(renderOptions, vulkanSwitch);
        addWeighted(renderOptions, hdrSwitch);
        addWeighted(renderOptions, debugSwitch);
        addWeighted(decodeOptions, zeroCopySwitch);
        addWeighted(decodeOptions, hardwareSwitch);
        controlsPanel.addView(renderOptions);
        controlsPanel.addView(decodeOptions);

        View.OnClickListener optionListener = view -> {
            updateOptionAvailability();
            applyHdrPreference();
            applyVideoSurfaceLayout();
            mainHandler.removeCallbacks(applyOptions);
            mainHandler.postDelayed(applyOptions, 250);
        };
        vulkanSwitch.setOnClickListener(optionListener);
        hdrSwitch.setOnClickListener(optionListener);
        zeroCopySwitch.setOnClickListener(optionListener);
        hardwareSwitch.setOnClickListener(optionListener);
        debugSwitch.setOnClickListener(view -> applyDebugVisibility());
        updateOptionAvailability();
        applyHdrPreference();
        applyDebugVisibility();

        setContentView(rootLayout);
    }

    private void setFullscreenMode(boolean fullscreen) {
        if (fullscreenMode == fullscreen
                || rootLayout == null
                || playerArea == null
                || controlsPanel == null) {
            return;
        }
        fullscreenMode = fullscreen;
        consumeFullscreenRevealGesture = false;
        ViewGroup parent = (ViewGroup) controlsPanel.getParent();
        if (parent != null) {
            parent.removeView(controlsPanel);
        }
        if (fullscreenMode) {
            controlsPanel.setBackgroundColor(Color.argb(192, 24, 24, 24));
            playerArea.addView(
                    controlsPanel,
                    new FrameLayout.LayoutParams(
                            FrameLayout.LayoutParams.MATCH_PARENT,
                            FrameLayout.LayoutParams.WRAP_CONTENT,
                            Gravity.BOTTOM));
            fullscreenButton.setText("Exit full");
            showFullscreenControls();
        } else {
            mainHandler.removeCallbacks(hideFullscreenControls);
            controlsPanel.setVisibility(View.VISIBLE);
            controlsPanel.setBackgroundColor(Color.rgb(32, 32, 32));
            rootLayout.addView(
                    controlsPanel,
                    new LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT));
            fullscreenButton.setText("Full screen");
        }
        applyFullscreenSystemUi();
        rootLayout.requestApplyInsets();
        playerArea.post(this::applyVideoSurfaceLayout);
    }

    private void showFullscreenControls() {
        if (!fullscreenMode || controlsPanel == null) {
            return;
        }
        controlsPanel.setVisibility(View.VISIBLE);
        scheduleFullscreenControlsHide();
    }

    private void scheduleFullscreenControlsHide() {
        mainHandler.removeCallbacks(hideFullscreenControls);
        if (fullscreenMode && !userSeeking) {
            mainHandler.postDelayed(
                    hideFullscreenControls,
                    FULLSCREEN_CONTROLS_TIMEOUT_MILLIS);
        }
    }

    private void applyFullscreenSystemUi() {
        if (fullscreenMode) {
            getWindow().addFlags(
                    WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        } else {
            getWindow().clearFlags(
                    WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    private void updateOptionAvailability() {
        // Vulkan off selects OpenGL ES. Keep that renderer preference
        // selectable even while MediaCodec is temporarily presenting directly
        // because ZeroCopy is off; it takes effect as soon as an application
        // renderer is selected again.
        vulkanSwitch.setEnabled(true);
        // Direct MediaCodec output bypasses the application renderer, but it
        // does not bypass Android's HDR Surface composition. HDR therefore
        // remains independent of the ZeroCopy interop choice.
        hdrSwitch.setEnabled(true);
        debugSwitch.setEnabled(true);
    }

    private void applyDebugVisibility() {
        if (statusView == null || debugSwitch == null) {
            return;
        }
        statusView.setVisibility(
                debugSwitch.isChecked() ? View.VISIBLE : View.GONE);
    }

    private void applyHdrPreference() {
        if (surfaceView == null || hdrSwitch == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 35) {
            // Zero asks Android to choose suitable HDR headroom for HDR
            // content. One requests no headroom above SDR white. Neither value
            // converts or retags a MediaCodec direct-Surface PQ/HLG buffer.
            // This API is specific to the SurfaceView layer and therefore also
            // applies when MediaCodec presents without Vulkan/OpenGL.
            surfaceView.setDesiredHdrHeadroom(
                    hdrSwitch.isChecked() ? 0.0f : 1.0f);
        }
    }

    private String buildOutputDiagnostics() {
        String output = nativeGetOutputColorSpace(nativeHandle);
        boolean directSurface = hardwareSwitch.isChecked()
                && !zeroCopySwitch.isChecked();
        String hdrPolicy;
        if (directSurface) {
            hdrPolicy = hdrSwitch.isChecked()
                    ? "auto headroom; codec passthrough"
                    : "1x headroom; codec passthrough";
        } else {
            hdrPolicy = hdrSwitch.isChecked() ? "PreferHdr" : "SdrOnly";
        }

        String firstLine = "Output " + output + " · HDR " + hdrPolicy;
        Display display = surfaceView.getDisplay();
        if (display == null) {
            return firstLine;
        }

        StringBuilder secondLine = new StringBuilder();
        if (Build.VERSION.SDK_INT >= 34
                && display.isHdrSdrRatioAvailable()) {
            try {
                float ratio = display.getHdrSdrRatio();
                if (!Float.isNaN(ratio)
                        && !Float.isInfinite(ratio)
                        && ratio >= 1.0f) {
                    secondLine.append(String.format(
                            Locale.US,
                            "HDR headroom %.2fx",
                            ratio));
                }
            } catch (IllegalStateException ignored) {
                // Availability can change while the display or Surface is
                // being recreated. The next 250 ms poll will try again.
            }
        }

        Display.HdrCapabilities capabilities = display.getHdrCapabilities();
        if (capabilities != null) {
            float targetMaxNits = capabilities.getDesiredMaxLuminance();
            if (!Float.isNaN(targetMaxNits)
                    && !Float.isInfinite(targetMaxNits)
                    && targetMaxNits > 0.0f) {
                if (secondLine.length() > 0) {
                    secondLine.append(" · ");
                }
                secondLine.append(String.format(
                        Locale.US,
                        "panel max target %.0f nits",
                        targetMaxNits));
            }
        }
        return secondLine.length() > 0
                ? firstLine + "\n" + secondLine
                : firstLine;
    }

    private void applyVideoSurfaceLayout() {
        if (playerArea == null || surfaceView == null) {
            return;
        }
        int targetWidth = FrameLayout.LayoutParams.MATCH_PARENT;
        int targetHeight = FrameLayout.LayoutParams.MATCH_PARENT;
        boolean directSurface = hardwareSwitch != null
                && hardwareSwitch.isChecked()
                && zeroCopySwitch != null
                && !zeroCopySwitch.isChecked();
        int availableWidth = playerArea.getWidth();
        int availableHeight = playerArea.getHeight();
        if (directSurface && videoWidth > 0 && videoHeight > 0
                && availableWidth > 0 && availableHeight > 0) {
            double scale = Math.min(
                    (double) availableWidth / videoWidth,
                    (double) availableHeight / videoHeight);
            targetWidth = Math.max(1, (int) Math.round(videoWidth * scale));
            targetHeight = Math.max(1, (int) Math.round(videoHeight * scale));
        }
        FrameLayout.LayoutParams layout =
                (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        if (layout.width == targetWidth
                && layout.height == targetHeight
                && layout.gravity == Gravity.CENTER) {
            return;
        }
        layout.width = targetWidth;
        layout.height = targetHeight;
        layout.gravity = Gravity.CENTER;
        surfaceView.setLayoutParams(layout);
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
        ParcelFileDescriptor descriptor = null;
        try {
            ContentResolver resolver = getContentResolver();
            descriptor = resolver.openFileDescriptor(uri, "r");
            if (descriptor == null) {
                throw new IllegalStateException(
                        "The document provider returned no descriptor");
            }
            String label = displayName(uri);
            int detachedDescriptor = descriptor.detachFd();
            descriptor.close();
            descriptor = null;
            knownDuration = 0;
            resetVideoSurfaceLayout();
            nativeOpenMedia(
                    nativeHandle,
                    label,
                    "",
                    detachedDescriptor);
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
        knownDuration = 0;
        resetVideoSurfaceLayout();
        nativeOpenMedia(
                nativeHandle,
                address,
                address,
                -1);
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

    private void resetVideoSurfaceLayout() {
        videoWidth = 0;
        videoHeight = 0;
        applyVideoSurfaceLayout();
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
        button.setTextSize(12.0f);
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

    private int currentDisplayRotation() {
        return getWindowManager().getDefaultDisplay().getRotation();
    }

    private void publishSurface(Surface surface) {
        int rotation = currentDisplayRotation();
        nativeSetSurface(nativeHandle, surface, rotation);
        publishedDisplayRotation = rotation;
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

    private native long nativeCreate(String caBundlePath);
    private native void nativeDestroy(long handle);
    private native void nativeSetSurface(
            long handle,
            Surface surface,
            int displayRotation);
    private native void nativeOpenMedia(
            long handle,
            String label,
            String path,
            int descriptor);
    private native void nativeSetOptions(
            long handle,
            boolean vulkan,
            boolean hdr,
            boolean zeroCopy,
            boolean hardwareDecode);
    private native void nativeTogglePlayback(long handle);
    private native void nativeStop(long handle);
    private native void nativeSeek(long handle, long position);
    private native long nativeGetPosition(long handle);
    private native long nativeGetDuration(long handle);
    private native long nativeGetVideoSize(long handle);
    private native boolean nativeApplyPendingVideoFallback(long handle);
    private native boolean nativeIsPlaying(long handle);
    private native int nativeGetRequestedFrameRate(long handle);
    private native String nativeGetStatus(long handle);
    private native String nativeGetOutputColorSpace(long handle);
}
