#ifdef __ANDROID__
#include <jni.h>
#include <string>
#include "LooperEngine.hpp"
#include "OboeBridge.hpp"

static LooperEngine* g_engine = nullptr;
static OboeLooperEngine* g_oboe_bridge = nullptr;

extern "C" {

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_startEngine(JNIEnv *env, jobject thiz) {
    if (!g_engine) {
        g_engine = new LooperEngine();
        g_oboe_bridge = new OboeLooperEngine(*g_engine);
        g_oboe_bridge->start();
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_stopEngine(JNIEnv *env, jobject thiz) {
    if (g_oboe_bridge) {
        g_oboe_bridge->stop();
        delete g_oboe_bridge;
        g_oboe_bridge = nullptr;
    }
    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_executeOverdub(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_overdub_command();
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_executeLoop(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_loop_command();
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_clearLoop(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_clear_command();
    }
}

// [חדש] העברת הפקודה לתוך הפיזיקה
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setDetectionMode(JNIEnv *env, jobject thiz, jint mode) {
    if (g_engine) {
        g_engine->set_detection_mode(mode);
    }
}

JNIEXPORT jfloat JNICALL
Java_com_notap_looper_AudioEngine_getEstimatedBPM(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        return g_engine->get_estimated_bpm();
    }
    return 0.0f;
}

JNIEXPORT jfloat JNICALL
Java_com_notap_looper_AudioEngine_getLoopPosition(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        return g_engine->get_loop_position();
    }
    return 0.0f;
}

JNIEXPORT jstring JNICALL
Java_com_notap_looper_AudioEngine_getCurrentState(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        std::string state_str = state_to_string(g_engine->get_current_state());
        return env->NewStringUTF(state_str.c_str());
    }
    return env->NewStringUTF("UNKNOWN");
}

}
#endif