package com.holy.game.core;

import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.widget.SwitchCompat;
import androidx.fragment.app.Fragment;

import com.holy.game.R;

import java.util.Locale;

/**
 * "Grafis" tab: controls of the EAGLE graphics engine (sun shadows).
 *
 * Every change goes to the native engine right away (applied on the next frame)
 * and is stored with {@link GraphicsNative#saveUserSettings()}, so the choice
 * survives a restart. The "Shader Uniform" rows are built from shaderUniform.ini
 * (like the SA_DOX uniform menu) and saved back to that file.
 * The reset button of the dialog calls {@link #resetToConfigFile()}: Config.ini
 * decides again and the shader values return to their defaults.
 */
public class DialogClientSettingsGraphicsFragment extends Fragment implements ISaveableFragment {
    private static final String TAG = "EagleGFX";

    private static final int DISTANCE_MIN = 40;
    private static final int DISTANCE_STEP = 10;
    private static final int DISTANCE_MAX = 300;

    private SwitchCompat mSwitchEnabled;
    private SwitchCompat mSwitchShadows;
    private SwitchCompat mSwitchDebugCascade;
    private SwitchCompat mSwitchDebugShadowMap;
    private SeekBar mSeekQuality;
    private SeekBar mSeekDistance;
    private TextView mTextQuality;
    private TextView mTextDistance;
    private TextView mTextStatus;
    private LinearLayout mUniforms;

    private View mRootView = null;
    private boolean bChangeAllowed = true;
    private boolean mNativeAvailable = true;
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    public static DialogClientSettingsGraphicsFragment createInstance(String txt) {
        return new DialogClientSettingsGraphicsFragment();
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(null);
    }

    @Override
    public void onViewStateRestored(@Nullable Bundle savedInstanceState) {
        super.onViewStateRestored(null);
    }

    @Override
    public void onSaveInstanceState(final Bundle outState) {
        outState.putSerializable("android:support:fragments", null);
        super.onSaveInstanceState(outState);
        outState.putSerializable("android:support:fragments", null);
    }

    @Override
    public View onCreateView(LayoutInflater inflater,
                             ViewGroup container,
                             Bundle savedInstanceState) {
        mRootView = inflater.inflate(R.layout.dialog_settings_graphics, container, false);

        mSwitchEnabled = mRootView.findViewById(R.id.switch_gfx_enabled);
        mSwitchShadows = mRootView.findViewById(R.id.switch_gfx_shadows);
        mSwitchDebugCascade = mRootView.findViewById(R.id.switch_gfx_debug_cascade);
        mSwitchDebugShadowMap = mRootView.findViewById(R.id.switch_gfx_debug_shadowmap);
        mSeekQuality = mRootView.findViewById(R.id.seek_gfx_quality);
        mSeekDistance = mRootView.findViewById(R.id.seek_gfx_shadow_distance);
        mTextQuality = mRootView.findViewById(R.id.text_gfx_quality_value);
        mTextDistance = mRootView.findViewById(R.id.text_gfx_shadow_distance_value);
        mTextStatus = mRootView.findViewById(R.id.text_gfx_status);
        mUniforms = mRootView.findViewById(R.id.layout_gfx_uniforms);

        mSeekQuality.setMax(GraphicsNative.QUALITY_ULTRA);
        mSeekDistance.setMax((DISTANCE_MAX - DISTANCE_MIN) / DISTANCE_STEP);

        getValues();

        mSwitchEnabled.setOnCheckedChangeListener((compoundButton, b) -> {
            if (!bChangeAllowed) return;
            call(() -> GraphicsNative.nativeSetGraphicsEnabled(b));
            changed();
        });
        mSwitchShadows.setOnCheckedChangeListener((compoundButton, b) -> {
            if (!bChangeAllowed) return;
            call(() -> GraphicsNative.nativeSetShadowEnabled(b));
            changed();
        });
        mSwitchDebugCascade.setOnCheckedChangeListener((compoundButton, b) -> {
            if (!bChangeAllowed) return;
            call(() -> GraphicsNative.nativeSetDebugFlag("showCascade", b));
            refreshStatus();
        });
        mSwitchDebugShadowMap.setOnCheckedChangeListener((compoundButton, b) -> {
            if (!bChangeAllowed) return;
            call(() -> GraphicsNative.nativeSetDebugFlag("showShadowMap", b));
            refreshStatus();
        });

        mSeekQuality.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                mTextQuality.setText(GraphicsNative.qualityName(progress));
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!bChangeAllowed) return;
                final int quality = GraphicsNative.clampQuality(seekBar.getProgress());
                call(() -> GraphicsNative.nativeSetGraphicsQuality(quality));
                // The quality preset also sets the shadow distance: show the new value.
                bChangeAllowed = false;
                setDistance(nativeDistance());
                bChangeAllowed = true;
                changed();
            }
        });

        mSeekDistance.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                mTextDistance.setText(String.format("%d m", progressToDistance(progress)));
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!bChangeAllowed) return;
                final int distance = progressToDistance(seekBar.getProgress());
                call(() -> GraphicsNative.nativeSetShadowDistance(distance));
                changed();
            }
        });

        mRootView.findViewById(R.id.button_gfx_reload).setOnClickListener(view -> {
            reloadFiles();
            Toast.makeText(getActivity(), "Config.ini dimuat ulang", Toast.LENGTH_SHORT).show();
        });

        return mRootView;
    }

    @Override
    public void onDestroyView() {
        mHandler.removeCallbacksAndMessages(null);
        super.onDestroyView();
    }

    @Override
    public void save() {
        if (mNativeAvailable) GraphicsNative.saveUserSettings();
    }

    @Override
    public void getValues() {
        if (mRootView == null) return;
        bChangeAllowed = false;
        try {
            mSwitchEnabled.setChecked(GraphicsNative.nativeIsGraphicsEnabled());
            mSwitchShadows.setChecked(GraphicsNative.nativeIsShadowEnabled());
            mSwitchDebugCascade.setChecked(GraphicsNative.nativeGetDebugFlag("showCascade"));
            mSwitchDebugShadowMap.setChecked(GraphicsNative.nativeGetDebugFlag("showShadowMap"));
            final int quality = GraphicsNative.clampQuality(GraphicsNative.nativeGetGraphicsQuality());
            mSeekQuality.setProgress(quality);
            mTextQuality.setText(GraphicsNative.qualityName(quality));
            setDistance(nativeDistance());
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
        }
        bChangeAllowed = true;
        buildUniformRows();
        refreshStatus();
    }

    /** "Muat ulang Config.ini": the files on the phone (edited by hand) decide again. */
    public void reloadFiles() {
        call(GraphicsNative::nativeReloadConfig);
        GraphicsNative.clearUserSettings();
        // The files are read by the game thread on its next frame.
        mHandler.postDelayed(this::getValues, 400);
    }

    /** Dialog reset button: Config.ini decides again, shader values back to the shader defaults. */
    public void resetToConfigFile() {
        call(() -> {
            GraphicsNative.nativeResetShaderUniforms();
            GraphicsNative.nativeSaveShaderUniforms();
        });
        reloadFiles();
    }

    // ---- shaderUniform.ini rows

    private void buildUniformRows() {
        if (mUniforms == null) return;
        mUniforms.removeAllViews();
        if (!mNativeAvailable) return;
        String list;
        try {
            list = GraphicsNative.nativeGetShaderUniforms();
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
            return;
        }
        if (list == null || list.isEmpty()) {
            mUniforms.addView(text("shaderUniform.ini kosong / belum dimuat", 11, 0x80FFFFFF, false));
            return;
        }
        String lastClass = null;
        for (String line : list.split("\n")) {
            final String[] f = line.split("\t");
            if (f.length < 8) continue;
            final String cls = f[0];
            final String name = f[1];
            final String type = f[2];
            final float value, min, max, step;
            try {
                value = Float.parseFloat(f[3]);
                min = Float.parseFloat(f[4]);
                max = Float.parseFloat(f[5]);
                step = Float.parseFloat(f[6]);
            } catch (NumberFormatException e) {
                continue;
            }
            final boolean used = "1".equals(f[7]);
            if (!cls.equals(lastClass)) {
                mUniforms.addView(text(cls, 12, 0xFFFCD32A, true));
                lastClass = cls;
            }
            final View row = "bool".equals(type)
                    ? boolRow(cls, name, value > 0.5f)
                    : numberRow(cls, name, "int".equals(type), value, min, max, step);
            if (!used) row.setAlpha(0.45f); // not referenced by any glShader file
            mUniforms.addView(row);
        }
    }

    private View boolRow(final String cls, final String name, boolean checked) {
        SwitchCompat sw = new SwitchCompat(requireContext());
        sw.setText(name);
        sw.setTextColor(0xFFFFFFFF);
        sw.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        sw.setChecked(checked);
        sw.setPadding(dp(10), dp(2), dp(10), dp(2));
        sw.setOnCheckedChangeListener((b, isChecked) -> {
            call(() -> GraphicsNative.nativeSetShaderUniform(cls, name, isChecked ? 1.0f : 0.0f));
            saveUniforms();
        });
        return sw;
    }

    private View numberRow(final String cls, final String name, final boolean integer, float value,
                           final float min, final float max, final float step) {
        LinearLayout row = new LinearLayout(requireContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        TextView label = text(name, 12, 0xFFFFFFFF, false);
        row.addView(label, new LinearLayout.LayoutParams(dp(130), ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView shown = text(formatValue(value, integer), 12, 0xFFFCD32A, false);
        final SeekBar bar = new SeekBar(requireContext());
        final float safeStep = step > 0.0f ? step : (integer ? 1.0f : 0.01f);
        bar.setMax(Math.max(1, Math.round((max - min) / safeStep)));
        bar.setProgress(Math.round((value - min) / safeStep));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) return;
                final float v = Math.min(max, min + progress * safeStep);
                shown.setText(formatValue(v, integer));
                call(() -> GraphicsNative.nativeSetShaderUniform(cls, name, v));
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) { saveUniforms(); }
        });
        row.addView(bar, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
        row.addView(shown, new LinearLayout.LayoutParams(dp(60), ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private void saveUniforms() {
        if (!mNativeAvailable) return;
        try {
            if (!GraphicsNative.nativeSaveShaderUniforms())
                Toast.makeText(getActivity(), "Gagal menyimpan shaderUniform.ini", Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
        }
    }

    private TextView text(String value, int sp, int color, boolean bold) {
        TextView t = new TextView(requireContext());
        t.setText(value);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(dp(10), dp(bold ? 8 : 4), dp(10), dp(4));
        return t;
    }

    private static String formatValue(float v, boolean integer) {
        return integer ? String.valueOf(Math.round(v)) : String.format(Locale.US, "%.2f", v);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void changed() {
        save();
        refreshStatus();
    }

    private void refreshStatus() {
        if (mTextStatus == null) return;
        if (!mNativeAvailable) {
            mTextStatus.setText("libmultiplayer.so tanpa EAGLE graphics engine");
            return;
        }
        try {
            final String status = GraphicsNative.nativeGetStatus();
            mTextStatus.setText(status != null ? status.replace(" ", "  ") : "");
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
        }
    }

    private void setDistance(int metres) {
        int progress = (metres - DISTANCE_MIN + DISTANCE_STEP / 2) / DISTANCE_STEP;
        progress = Math.max(0, Math.min(mSeekDistance.getMax(), progress));
        mSeekDistance.setProgress(progress);
        mTextDistance.setText(String.format("%d m", progressToDistance(progress)));
    }

    private static int progressToDistance(int progress) {
        return DISTANCE_MIN + progress * DISTANCE_STEP;
    }

    private int nativeDistance() {
        try {
            return Math.round(GraphicsNative.nativeGetShadowDistance());
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
            return 160;
        }
    }

    private void call(Runnable nativeCall) {
        if (!mNativeAvailable) return;
        try {
            nativeCall.run();
        } catch (UnsatisfiedLinkError e) {
            nativeMissing(e);
        }
    }

    private void nativeMissing(UnsatisfiedLinkError e) {
        if (!mNativeAvailable) return;
        mNativeAvailable = false;
        Log.w(TAG, "GraphicsNative not found in libmultiplayer.so", e);
        if (mRootView == null) return;
        mSwitchEnabled.setEnabled(false);
        mSwitchShadows.setEnabled(false);
        mSwitchDebugCascade.setEnabled(false);
        mSwitchDebugShadowMap.setEnabled(false);
        mSeekQuality.setEnabled(false);
        mSeekDistance.setEnabled(false);
        if (mUniforms != null) mUniforms.removeAllViews();
    }
}
