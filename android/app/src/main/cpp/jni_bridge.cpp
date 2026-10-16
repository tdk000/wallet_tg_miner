// ===========================================================================
//  jni_bridge.cpp - thin JNI layer over the wallet_tg_miner C API.
//
//  All cryptography lives in the shared core (wallet_tg_miner.cpp +
//  monocypher.c); this file only marshals arguments and results.
// ===========================================================================
#include <jni.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "miner_api.h"

namespace {

std::string jstr(JNIEnv *env, jstring s){
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

jobjectArray mkarray(JNIEnv *env, const std::string *fields, int n){
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(n, strCls, nullptr);
    if (!arr) return nullptr;
    for (int i = 0; i < n; i++){
        jstring s = env->NewStringUTF(fields[i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

} // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_wallettg_miner_Native_run(
        JNIEnv *env, jclass,
        jstring jmode, jstring jscheme, jstring jsuffix, jstring jprefix,
        jint words, jint threads, jboolean gpu, jboolean testnet, jboolean ci){

    // Keep the backing strings alive for the whole call.
    std::string mode   = jstr(env, jmode);
    std::string scheme = jstr(env, jscheme);
    std::string suffix = jstr(env, jsuffix);
    std::string prefix = jstr(env, jprefix);

    wtm_params p;
    wtm_defaults(&p);
    p.mode   = mode.c_str();
    p.scheme = scheme.c_str();
    p.suffix = suffix.c_str();
    p.prefix = prefix.c_str();
    p.words  = words;
    p.threads= threads;
    p.gpu    = gpu ? 1 : 0;
    p.testnet= testnet ? 1 : 0;
    p.ci     = ci ? 1 : 0;

    wtm_result r;
    int rc = wtm_run(&p, &r);

    const char *status = (rc == 1) ? "found" : (rc == 0 ? "stopped" : "error");

    char sub[32]; snprintf(sub, sizeof(sub), "0x%08X", (unsigned)r.subwalletId);
    char att[32]; snprintf(att, sizeof(att), "%llu", (unsigned long long)r.attempts);
    char el[32];  snprintf(el,  sizeof(el),  "%.1f", r.elapsed);

    std::string fields[11] = {
        status,
        r.address,
        r.mnemonic,
        r.raw,
        r.pubkey,
        sub,
        att,
        el,
        r.error,
        r.usesGpu ? "1" : "0",
        r.importable ? "1" : "0",
    };

    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(11, strCls, nullptr);
    if (!arr) return nullptr;
    for (int i = 0; i < 11; i++){
        jstring s = env->NewStringUTF(fields[i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_wallettg_miner_Native_stop(JNIEnv *, jclass){
    wtm_request_stop();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_wallettg_miner_Native_attempts(JNIEnv *, jclass){
    return (jlong)wtm_attempts();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wallettg_miner_Native_selftest(JNIEnv *, jclass){
    return wtm_selftest() ? JNI_TRUE : JNI_FALSE;
}

// Old insecure deterministic fallback: does this phrase appear in its stream?
// Returns [status, index, error] with status "found" | "notfound" | "error".
// The phrase is wiped from local memory and never logged.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_wallettg_miner_Native_checkFallbackPhrase(
        JNIEnv *env, jclass, jstring jmnemonic, jlong limit){
    std::string m = jstr(env, jmnemonic);
    wtm_fallback_result r;
    int rc = wtm_check_fallback_phrase(m.c_str(), (unsigned long long)limit, &r);
    if (!m.empty()) memset(&m[0], 0, m.size());

    const char *status = (rc == 1) ? "found" : (rc == 0 ? "notfound" : "error");
    char idx[32]; snprintf(idx, sizeof(idx), "%llu", (unsigned long long)r.index);
    std::string fields[3] = { status, idx, r.error };
    return mkarray(env, fields, 3);
}

// Developer diagnostics: reproduce the phrase/address the old buggy fallback
// would emit at `index`. Returns [status, index, mnemonic, address, error].
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_wallettg_miner_Native_insecureDemoFallback(JNIEnv *env, jclass, jlong index){
    wtm_fallback_result r;
    int rc = wtm_insecure_demo_fallback((unsigned long long)index, &r);

    const char *status = (rc == 1) ? "ok" : "error";
    char idx[32]; snprintf(idx, sizeof(idx), "%llu", (unsigned long long)r.index);
    std::string fields[5] = { status, idx, r.mnemonic, r.address, r.error };
    return mkarray(env, fields, 5);
}
