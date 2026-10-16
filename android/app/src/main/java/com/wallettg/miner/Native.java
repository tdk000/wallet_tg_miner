package com.wallettg.miner;

/** Thin binding to the native wallet_tg_miner core. */
public final class Native {

    static {
        System.loadLibrary("wallet_tg_miner");
    }

    private Native() {}

    /**
     * Runs a search to completion (blocking — call from a background thread).
     *
     * @param mode   "phrase" or "subwallet"
     * @param scheme "app" (Telegram-compatible) or "ton"
     * @return array of 11 strings: status, address, mnemonic, raw, pubkey,
     *         subwalletId, attempts, elapsed, error, usesGpu, importable.
     *         status is "found", "stopped" or "error".
     */
    public static native String[] run(String mode, String scheme, String suffix,
                                      String prefix, int words, int threads,
                                      boolean gpu, boolean testnet, boolean ci);

    /** Requests a cooperative stop of the running search. */
    public static native void stop();

    /** Attempts performed by the running (or last) search. */
    public static native long attempts();

    /** Runs the built-in cryptographic self-tests. */
    public static native boolean selftest();

    /**
     * Checks whether a 12-word mnemonic came from the old insecure
     * deterministic fallback.  The phrase is not stored or logged.
     *
     * @return array of 3 strings: status, index, error, where status is
     *         "found", "notfound" or "error".
     */
    public static native String[] checkFallbackPhrase(String mnemonic, long limit);

    /**
     * Developer mode: reproduces the phrase/address the old insecure fallback
     * would have produced at {@code index}.  Never use such a phrase for funds.
     *
     * @return array of 5 strings: status, index, mnemonic, address, error.
     */
    public static native String[] insecureDemoFallback(long index);
}
