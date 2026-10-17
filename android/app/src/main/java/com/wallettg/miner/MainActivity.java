package com.wallettg.miner;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Locale;

public class MainActivity extends Activity {

    private static final String TAG = "WalletTgMiner";

    private EditText pattern;
    private RadioGroup placeGroup, modeGroup;
    private Spinner words;
    private CheckBox ci, gpu;
    private Button start, stop, selftest;
    private TextView progress, resultBody;
    private View resultCard;
    private Button copyAddr, copyPhrase;

    // Old-version weak-phrase check + developer-only demo.
    private EditText checkPhrase, checkLimit, demoIndex;
    private Button checkRun, demoRun;
    private TextView checkResult, demoResult;
    private CheckBox devMode;
    private View devPanel;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean running = false;
    private Thread worker;
    private long t0;
    private double expected;

    private String lastAddress = "";
    private String lastMnemonic = "";

    private final Runnable ticker = new Runnable() {
        @Override public void run() {
            if (!running) return;
            long a = Native.attempts();
            double el = (System.currentTimeMillis() - t0) / 1000.0;
            double rate = el > 0 ? a / el : 0;
            String pct = expected > 1.0
                    ? String.format(Locale.US, "\n" + getString(R.string.chance_fmt), 100.0 * (1.0 - Math.exp(-a / expected)))
                    : "";
            progress.setText(String.format(Locale.US,
                    getString(R.string.progress_fmt), a, rate, el, pct));
            ui.postDelayed(this, 500);
        }
    };

    private static final String PREFS = "wtm";
    private static final String KEY_LANG = "lang";

    @Override
    protected void attachBaseContext(Context newBase) {
        super.attachBaseContext(applyLocale(newBase));
    }

    private static Context applyLocale(Context ctx) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String lang = p.getString(KEY_LANG, "");
        if (lang.isEmpty()) return ctx;
        Locale loc;
        if ("ru".equals(lang)) loc = new Locale("ru");
        else if ("zh".equals(lang)) loc = new Locale("zh");
        else loc = new Locale("en");
        Configuration c = new Configuration(ctx.getResources().getConfiguration());
        c.setLocale(loc);
        return ctx.createConfigurationContext(c);
    }

    private void maybeAskLanguage() {
        SharedPreferences p = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (!p.getString(KEY_LANG, "").isEmpty()) return;
        if (getIntent() != null && getIntent().getExtras() != null && !getIntent().getExtras().isEmpty()) return;
        String[] items = { getString(R.string.lang_ru), getString(R.string.lang_en), getString(R.string.lang_zh) };
        new AlertDialog.Builder(this)
                .setTitle(R.string.lang_title)
                .setItems(items, (d, which) -> {
                    String code = (which == 0) ? "ru" : (which == 1) ? "en" : "zh";
                    p.edit().putString(KEY_LANG, code).apply();
                    recreate();
                })
                .setCancelable(false)
                .show();
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        pattern    = findViewById(R.id.pattern);
        placeGroup = findViewById(R.id.place);
        modeGroup  = findViewById(R.id.mode);
        words      = findViewById(R.id.words);
        ci         = findViewById(R.id.ci);
        gpu        = findViewById(R.id.gpu);
        start      = findViewById(R.id.start);
        stop       = findViewById(R.id.stop);
        selftest   = findViewById(R.id.selftest);
        progress   = findViewById(R.id.progress);
        resultCard = findViewById(R.id.resultCard);
        resultBody = findViewById(R.id.resultBody);
        copyAddr   = findViewById(R.id.copyAddr);
        copyPhrase = findViewById(R.id.copyPhrase);

        checkPhrase = findViewById(R.id.checkPhrase);
        checkLimit  = findViewById(R.id.checkLimit);
        demoIndex   = findViewById(R.id.demoIndex);
        checkRun    = findViewById(R.id.checkRun);
        demoRun     = findViewById(R.id.demoRun);
        checkResult = findViewById(R.id.checkResult);
        demoResult  = findViewById(R.id.demoResult);
        devMode     = findViewById(R.id.devMode);
        devPanel    = findViewById(R.id.devPanel);

        ArrayAdapter<String> ad = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_item,
                getResources().getStringArray(R.array.words_entries));
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        words.setAdapter(ad);

        modeGroup.setOnCheckedChangeListener((g, id) -> refreshEnabledState());
        placeGroup.setOnCheckedChangeListener((g, id) -> refreshEnabledState());

        start.setOnClickListener(v -> startSearch());
        stop.setOnClickListener(v -> Native.stop());
        selftest.setOnClickListener(v -> runSelftest());

        copyAddr.setOnClickListener(v -> copyToClipboard("WalletTg address", lastAddress));
        copyPhrase.setOnClickListener(v -> copyToClipboard("WalletTg mnemonic", lastMnemonic));

        checkRun.setOnClickListener(v -> runFallbackCheck());
        demoRun.setOnClickListener(v -> runDemoFallback());
        devMode.setOnCheckedChangeListener((b, on) ->
                devPanel.setVisibility(on ? View.VISIBLE : View.GONE));

        maybeRunFromIntent();
        maybeAskLanguage();
    }

    private boolean phraseMode() {
        return modeGroup.getCheckedRadioButtonId() != R.id.modeSub;
    }

    private void refreshEnabledState() {
        if (running) return;
        boolean phrase = phraseMode();
        words.setEnabled(phrase);
        gpu.setEnabled(phrase);
    }

    private void runSelftest() {
        boolean ok = Native.selftest();
        Log.i(TAG, "WTM_SELFTEST " + (ok ? "ok" : "fail"));
        toast(ok ? getString(R.string.selftest_ok) : getString(R.string.selftest_fail));
    }

    /**
     * Headless entry points, handy for scripting and automated checks:
     *   adb shell am start -n com.wallettg.miner/.MainActivity --ez selftest true
     *   adb shell am start -n com.wallettg.miner/.MainActivity --es pattern qq --es mode sub
     */
    private void maybeRunFromIntent() {
        Intent it = getIntent();
        if (it == null) return;

        if (it.getBooleanExtra("selftest", false)) { runSelftest(); return; }

        String p = it.getStringExtra("pattern");
        if (p == null || p.isEmpty()) return;

        placeGroup.check("start".equals(it.getStringExtra("place")) ? R.id.placeStart : R.id.placeEnd);
        modeGroup.check("sub".equals(it.getStringExtra("mode")) ? R.id.modeSub : R.id.modePhrase);
        pattern.setText(p);
        ci.setChecked(it.getBooleanExtra("ci", false));
        gpu.setChecked(it.getBooleanExtra("gpu", false));
        if (it.getIntExtra("words", 12) == 24) words.setSelection(1);
        startSearch();
    }

    private void startSearch() {
        if (running) return;

        final String pat = pattern.getText().toString().trim();
        if (pat.isEmpty()) { toast(getString(R.string.err_empty_pattern)); return; }

        for (int i = 0; i < pat.length(); i++) {
            char c = pat.charAt(i);
            boolean ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) { toast(getString(R.string.err_bad_chars)); return; }
        }

        final boolean atEnd = placeGroup.getCheckedRadioButtonId() == R.id.placeEnd;
        final boolean phrase = phraseMode();
        final String m = phrase ? "phrase" : "subwallet";
        final String suffix = atEnd ? pat : "";
        final String prefix = atEnd ? "" : pat;
        final int nWords = phrase && words.getSelectedItemPosition() == 1 ? 24 : 12;
        final boolean fCi = ci.isChecked();
        final boolean fGpu = phrase && gpu.isChecked();

        expected = 1.0;
        for (int i = 0; i < pat.length(); i++) {
            char c = pat.charAt(i);
            boolean alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            expected *= (fCi && alpha) ? 32.0 : 64.0;
        }

        setBusy(true);
        running = true;
        t0 = System.currentTimeMillis();
        progress.setText(getString(R.string.starting) + (fGpu ? getString(R.string.starting_gpu) : getString(R.string.starting_cpu)));
        resultCard.setVisibility(View.GONE);
        Log.i(TAG, "WTM_START mode=" + m + " (atEnd=" + atEnd + ") pattern=" + pat);
        ui.post(ticker);

        worker = new Thread(() -> {
            String[] r = Native.run(m, "app", suffix, prefix, nWords, 0, fGpu, false, fCi);
            ui.post(() -> onResult(r));
        }, "miner");
        worker.start();
    }

    private void onResult(String[] r) {
        running = false;
        setBusy(false);

        if (r == null) {
            progress.setText(getString(R.string.internal_error));
            Log.i(TAG, "WTM_RESULT error (null)");
            return;
        }

        String status = r[0];
        if ("error".equals(status)) {
            progress.setText(String.format(getString(R.string.error_fmt), r[8]));
            Log.i(TAG, "WTM_RESULT error: " + r[8]);
            return;
        }

        long attempts = 0;
        double elapsed = 0;
        try { attempts = Long.parseLong(r[6]); } catch (Exception ignored) {}
        try { elapsed = Double.parseDouble(r[7]); } catch (Exception ignored) {}

        if ("stopped".equals(status)) {
            progress.setText(String.format(Locale.US,
                    getString(R.string.stopped_fmt), attempts, elapsed));
            Log.i(TAG, "WTM_RESULT stopped attempts=" + attempts);
            return;
        }

        // found
        lastAddress = r[1];
        lastMnemonic = r[2];
        boolean importable = "1".equals(r[10]);
        Log.i(TAG, "WTM_RESULT found address=" + r[1] + " subwallet=" + r[5]
                + " importable=" + (importable ? "1" : "0")
                + " attempts=" + attempts + " mnemonicWords=" + (r[2].isEmpty() ? 0 : r[2].split(" ").length));

        StringBuilder sb = new StringBuilder();
        sb.append(getString(R.string.address_label)).append(r[1]).append("\n\n");
        if (!r[2].isEmpty()) sb.append(getString(R.string.mnemonic_label)).append(r[2]).append("\n\n");
        sb.append("subwalletId: ").append(r[5]).append("\n");
        sb.append(getString(R.string.attempts_label)).append(String.format(Locale.US, "%,d", attempts)).append("\n");
        sb.append(String.format(Locale.US, getString(R.string.time_fmt), elapsed));
        if ("1".equals(r[9])) sb.append("\n").append(getString(R.string.gpu_yes));
        sb.append("\n\n");
        sb.append(importable
                ? getString(R.string.found_importable)
                : getString(R.string.found_nonstandard));

        resultBody.setText(sb.toString());
        resultCard.setVisibility(View.VISIBLE);
        progress.setText(String.format(Locale.US, getString(R.string.done_fmt), attempts, elapsed));
    }

    // ---- old-version weak-phrase check ------------------------------------

    /**
     * Checks whether a phrase was produced by the old insecure deterministic
     * fallback.  The phrase is never logged, never copied and the input field
     * is cleared before the check starts.
     */
    private void runFallbackCheck() {
        final String phrase = checkPhrase.getText().toString().trim();
        if (phrase.isEmpty()) { toast(getString(R.string.enter_12_words)); return; }

        long limit = 100000;
        try { limit = Long.parseLong(checkLimit.getText().toString().trim()); } catch (Exception ignored) {}
        if (limit <= 0) limit = 100000;

        checkPhrase.setText("");                 // keep the secret out of the UI
        checkResult.setText(getString(R.string.checking));
        final long fLimit = limit;

        new Thread(() -> {
            String[] r = Native.checkFallbackPhrase(phrase, fLimit);
            ui.post(() -> onCheckResult(r));
        }, "fallback-check").start();
    }

    private void onCheckResult(String[] r) {
        if (r == null) { checkResult.setText(getString(R.string.internal_error)); return; }
        if ("error".equals(r[0])) { checkResult.setText(String.format(getString(R.string.error_fmt), r[2])); return; }
        if ("found".equals(r[0])) {
            checkResult.setText(String.format(getString(R.string.check_found_fmt), r[1]));
            return;
        }
        checkResult.setText(getString(R.string.check_notfound));
    }

    /** Developer mode only: reproduce an insecure fallback phrase (never for funds). */
    private void runDemoFallback() {
        long idx = 0;
        try { idx = Long.parseLong(demoIndex.getText().toString().trim()); } catch (Exception ignored) {}
        if (idx < 0) idx = 0;

        String[] r = Native.insecureDemoFallback(idx);
        if (r == null || !"ok".equals(r[0])) {
            demoResult.setText(String.format(getString(R.string.demo_error_fmt), (r == null ? "null" : r[4])));
            return;
        }
        demoResult.setText(getString(R.string.demo_title) + "\n\n"
                + getString(R.string.demo_phrase_label) + r[1] + "\n" + r[2] + "\n\n"
                + getString(R.string.demo_address_label) + r[3]);
    }

    private void setBusy(boolean busy) {
        start.setEnabled(!busy);
        stop.setEnabled(busy);
        pattern.setEnabled(!busy);
        modeGroup.setEnabled(!busy);
        placeGroup.setEnabled(!busy);
        ci.setEnabled(!busy);
        words.setEnabled(!busy && phraseMode());
        gpu.setEnabled(!busy && phraseMode());
    }

    @Override protected void onDestroy() {
        Native.stop();
        super.onDestroy();
    }

    private void copyToClipboard(String label, String text) {
        if (text == null || text.isEmpty()) { toast(getString(R.string.nothing_to_copy)); return; }
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText(label, text));
        toast(getString(R.string.copied));
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }
}
