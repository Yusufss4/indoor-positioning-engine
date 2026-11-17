// JNI bridge of io.github.yusufss4.ipe.PositioningEngine: one ipe::LiveSession per Kotlin object. The
// session is the engine plus the recording of everything it was fed (core/include/ipe/live_session.hpp).
//
// Text that can hold any character (the venue, a config patch, the trace) crosses as UTF-8 bytes, because
// JNI's own strings are "modified UTF-8". Ids and metadata cross as JNI strings.

#include <jni.h>

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "ipe/live_session.hpp"
#include "ipe/venue.hpp"

namespace {

ipe::LiveSession& session(jlong handle) { return *reinterpret_cast<ipe::LiveSession*>(handle); }

std::string bytes(JNIEnv* env, jbyteArray array) {
  if (array == nullptr) return {};
  const jsize n = env->GetArrayLength(array);
  std::string out(static_cast<std::size_t>(n), '\0');
  env->GetByteArrayRegion(array, 0, n, reinterpret_cast<jbyte*>(out.data()));
  return out;
}

std::string text(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars != nullptr ? chars : "");
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

/// A C++ exception becomes an IllegalArgumentException: a venue or a config patch that does not parse.
void rethrow(JNIEnv* env, const std::exception& e) {
  if (const jclass cls = env->FindClass("java/lang/IllegalArgumentException")) env->ThrowNew(cls, e.what());
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeCreate(JNIEnv* env, jclass,
                                                                                   jbyteArray venue_json,
                                                                                   jbyteArray config_patch) {
  try {
    auto* s = new ipe::LiveSession(ipe::parse_venue(bytes(env, venue_json)), bytes(env, config_patch));
    s->set_metadata("source", "android");
    return reinterpret_cast<jlong>(s);
  } catch (const std::exception& e) {
    rethrow(env, e);
    return 0;
  }
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<ipe::LiveSession*>(handle);
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeOnImu(JNIEnv*, jclass, jlong handle,
                                                                                 jdouble t, jdouble ax, jdouble ay,
                                                                                 jdouble az, jdouble gx, jdouble gy,
                                                                                 jdouble gz, jdouble mx, jdouble my,
                                                                                 jdouble mz) {
  session(handle).on_imu(t, {ax, ay, az}, {gx, gy, gz}, {mx, my, mz});
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeOnBle(JNIEnv* env, jclass, jlong handle,
                                                                                 jdouble t, jstring id, jdouble rssi) {
  session(handle).on_ble(t, text(env, id), rssi);
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeOnScan(JNIEnv* env, jclass, jlong handle,
                                                                                  jdouble t, jobjectArray ids,
                                                                                  jdoubleArray rssi) {
  const jsize n = env->GetArrayLength(ids);
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(n));
  for (jsize i = 0; i < n; ++i) {
    const auto id = static_cast<jstring>(env->GetObjectArrayElement(ids, i));
    names.push_back(text(env, id));
    env->DeleteLocalRef(id);
  }
  std::vector<jdouble> dbm(static_cast<std::size_t>(n));
  env->GetDoubleArrayRegion(rssi, 0, n, dbm.data());
  std::vector<ipe::LiveSession::Reading> readings;
  readings.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) readings.push_back({names[i], dbm[i]});
  session(handle).on_scan(t, readings);
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeOnPressure(JNIEnv*, jclass, jlong handle,
                                                                                      jdouble t, jdouble hpa) {
  session(handle).on_pressure(t, hpa);
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeOnPositionFix(JNIEnv*, jclass, jlong handle,
                                                                                         jdouble t, jdouble x,
                                                                                         jdouble y, jint level,
                                                                                         jdouble sigma_m) {
  session(handle).on_position_fix(t, {x, y}, level, sigma_m);
}

JNIEXPORT jdouble JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeMark(JNIEnv*, jclass, jlong handle,
                                                                                   jdouble t, jdouble x, jdouble y,
                                                                                   jint level) {
  return session(handle).mark(t, {x, y}, level);
}

/// out = {valid, x, y, level, sigma radius, heading}.
JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeEstimate(JNIEnv* env, jclass, jlong handle,
                                                                                    jdoubleArray out) {
  const ipe::LiveSession& s = session(handle);
  const ipe::Estimate e = s.estimate();
  const jdouble values[6] = {e.valid ? 1.0 : 0.0,           e.pos.x,          e.pos.y,
                             static_cast<jdouble>(e.level), e.sigma_radius(), s.engine().raw_estimate().heading};
  env->SetDoubleArrayRegion(out, 0, 6, values);
}

JNIEXPORT jint JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeLevelCount(JNIEnv*, jclass, jlong handle) {
  return session(handle).venue().level_count();
}

JNIEXPORT jint JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeSteps(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(session(handle).engine().step_count());
}

JNIEXPORT jdouble JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeTime(JNIEnv*, jclass, jlong handle) {
  return session(handle).time();
}

JNIEXPORT void JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeSetMetadata(JNIEnv* env, jclass,
                                                                                       jlong handle, jstring key,
                                                                                       jstring value) {
  session(handle).set_metadata(text(env, key), text(env, value));
}

JNIEXPORT jbyteArray JNICALL Java_io_github_yusufss4_ipe_PositioningEngine_nativeTrace(JNIEnv* env, jclass,
                                                                                       jlong handle) {
  const std::string csv = session(handle).trace_csv();
  const jbyteArray out = env->NewByteArray(static_cast<jsize>(csv.size()));
  if (out != nullptr) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(csv.size()), reinterpret_cast<const jbyte*>(csv.data()));
  }
  return out;
}

}  // extern "C"
