#pragma once

#include <assert.h>
#include <jni.h>
#include <js.h>
#include <stdint.h>

#include <string>
#include <unordered_map>

static const js_type_tag_t bare_jni__carrier = {0xd049b8d33df9dd47, 0x1527338b76e78224};

typedef struct {
  jobject object;
  js_ref_t *wrapper;
  uint32_t claims;
} bare_jni_entry_t;

typedef struct {
  uint32_t tag;
  js_ref_t *wrapper;
} bare_jni__claim_t;

// The VM belongs to the whole process, not to a registry.
static JavaVM *bare_jni__vm = nullptr;
static jclass bare_jni__system = nullptr;
static jmethodID bare_jni__identity_hash_code = nullptr;

typedef struct {
  std::unordered_map<uint32_t, bare_jni_entry_t> entries;
  std::unordered_multimap<jint, uint32_t> tags;

  uint32_t next_tag;
  uint32_t refs;
} bare_jni_registry_t;

/**
 * Remember the Java VM that `env` belongs to. Call this once before using any
 * registry. Calling it again does nothing.
 */
static void
bare_jni_attach(JNIEnv *env) {
  if (bare_jni__vm) return;

  int err = env->GetJavaVM(&bare_jni__vm);
  assert(err == JNI_OK);

  jclass system = env->FindClass("java/lang/System");
  assert(system);

  bare_jni__system = static_cast<jclass>(env->NewGlobalRef(system));

  bare_jni__identity_hash_code = env->GetStaticMethodID(bare_jni__system, "identityHashCode", "(Ljava/lang/Object;)I");
  assert(bare_jni__identity_hash_code);

  env->DeleteLocalRef(system);
}

/**
 * Return the `JNIEnv` of the current thread. JavaScript must run on a thread
 * that is attached to the Java VM.
 */
static JNIEnv *
bare_jni_env(void) {
  assert(bare_jni__vm);

  JNIEnv *env;

  int err = bare_jni__vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  assert(err == JNI_OK);

  return env;
}

/**
 * Keep `registry` alive, for example while native code holds on to it. Give the
 * reference back with `bare_jni_registry_release()`.
 */
static void
bare_jni_registry_retain(bare_jni_registry_t *registry) {
  registry->refs++;
}

/**
 * Give back a reference to `registry`. The registry is freed when the last
 * reference is gone.
 */
static void
bare_jni_registry_release(bare_jni_registry_t *registry) {
  assert(registry->refs > 0);

  if (--registry->refs > 0) return;

  JNIEnv *jni = bare_jni_env();

  for (auto &entry : registry->entries) {
    jni->DeleteGlobalRef(entry.second.object);
  }

  delete registry;
}

static void
bare_jni__on_registry_release(js_env_t *env, void *data, void *finalize_hint) {
  bare_jni_registry_release(static_cast<bare_jni_registry_t *>(data));
}

/**
 * Create a registry for an addon, and pass it as the data pointer of every
 * function the addon exports. The registry lives as long as `exports` and every
 * token it has handed out.
 */
static bare_jni_registry_t *
bare_jni_registry_create(js_env_t *env, js_value_t *exports) {
  int err;

  auto registry = new bare_jni_registry_t();

  registry->next_tag = 1;
  registry->refs = 1;

  err = js_add_finalizer(env, exports, registry, bare_jni__on_registry_release, NULL, NULL);
  assert(err == 0);

  return registry;
}

static jint
bare_jni__identity(JNIEnv *env, jobject object) {
  return env->CallStaticIntMethod(bare_jni__system, bare_jni__identity_hash_code, object);
}

static std::string
bare_jni__name(JNIEnv *env, jclass clazz) {
  jclass meta = env->GetObjectClass(clazz);

  jmethodID get_name = env->GetMethodID(meta, "getName", "()Ljava/lang/String;");
  assert(get_name);

  auto name = static_cast<jstring>(env->CallObjectMethod(clazz, get_name));

  const char *utf = env->GetStringUTFChars(name, nullptr);

  std::string result(utf);

  env->ReleaseStringUTFChars(name, utf);

  env->DeleteLocalRef(name);
  env->DeleteLocalRef(meta);

  return result;
}

static std::string
bare_jni__class_name(JNIEnv *env, jobject object) {
  jclass clazz = env->GetObjectClass(object);

  std::string result = bare_jni__name(env, clazz);

  env->DeleteLocalRef(clazz);

  return result;
}

/**
 * Return the tag of `object`, or 0 if it has none. Two references to the same
 * Java object are not equal, so objects are found by identity hash and compared
 * with `IsSameObject`.
 */
static uint32_t
bare_jni_find(bare_jni_registry_t *registry, JNIEnv *env, jobject object) {
  auto range = registry->tags.equal_range(bare_jni__identity(env, object));

  for (auto it = range.first; it != range.second; it++) {
    auto entry = registry->entries.find(it->second);

    if (entry == registry->entries.end()) continue;

    if (env->IsSameObject(entry->second.object, object)) return it->second;
  }

  return 0;
}

/**
 * Give `object` a tag and return it. An object that already has a tag keeps it,
 * even if `object` is a different reference to it.
 */
static uint32_t
bare_jni_tag(bare_jni_registry_t *registry, jobject object) {
  JNIEnv *env = bare_jni_env();

  uint32_t existing = bare_jni_find(registry, env, object);

  if (existing) return existing;

  uint32_t tag = registry->next_tag++;

  registry->entries[tag] = {env->NewGlobalRef(object), nullptr, 0};
  registry->tags.emplace(bare_jni__identity(env, object), tag);

  return tag;
}

static bare_jni_entry_t *
bare_jni__entry(bare_jni_registry_t *registry, uint32_t tag) {
  auto entry = registry->entries.find(tag);

  return entry == registry->entries.end() ? nullptr : &entry->second;
}

static js_value_t *
bare_jni__wrapper(js_env_t *env, bare_jni_entry_t *entry) {
  if (entry->wrapper == NULL) return NULL;

  js_value_t *result;
  int err = js_get_reference_value(env, entry->wrapper, &result);
  assert(err == 0);

  return result;
}

/**
 * Return the object for `tag`, or `nullptr` if there is none.
 */
static jobject
bare_jni_object(bare_jni_registry_t *registry, uint32_t tag) {
  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  if (entry == nullptr) return nullptr;

  return entry->object;
}

static int
bare_jni__read_uint32(js_env_t *env, js_value_t *value, const char *name, uint32_t *result) {
  int err;

  bool is;
  err = js_is_number(env, value, &is);
  assert(err == 0);

  if (!is) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be a number", name);
    assert(err == 0);

    return -1;
  }

  err = js_get_value_uint32(env, value, result);
  assert(err == 0);

  return 0;
}

/**
 * Read the object for the tag in `value` into `result` and return 0. If `value`
 * is not a number or not a known tag, throw a JavaScript error that mentions
 * `name` and return -1.
 */
static int
bare_jni_read_tag(js_env_t *env, bare_jni_registry_t *registry, js_value_t *value, const char *name, jobject *result) {
  int err;

  uint32_t tag;
  err = bare_jni__read_uint32(env, value, name, &tag);
  if (err < 0) return err;

  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  if (entry == nullptr) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return -1;
  }

  *result = entry->object;

  return 0;
}

/**
 * Like `bare_jni_read_tag()`, but also throw a `TypeError` and return -1 if the
 * object is not an instance of `type`. The caller passes the class in, because
 * `FindClass` cannot see the app's own classes.
 */
static int
bare_jni_read_type(js_env_t *env, bare_jni_registry_t *registry, js_value_t *value, const char *name, jclass type, jobject *result) {
  int err;

  jobject object;
  err = bare_jni_read_tag(env, registry, value, name, &object);
  if (err < 0) return err;

  JNIEnv *jni = bare_jni_env();

  if (!jni->IsInstanceOf(object, type)) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be %s, not %s", name, bare_jni__name(jni, type).c_str(), bare_jni__class_name(jni, object).c_str());
    assert(err == 0);

    return -1;
  }

  *result = object;

  return 0;
}

/**
 * Return the JavaScript wrapper of `object`, or `NULL` if it has none.
 */
static js_value_t *
bare_jni_lookup(js_env_t *env, bare_jni_registry_t *registry, jobject object) {
  uint32_t tag = bare_jni_find(registry, bare_jni_env(), object);

  if (tag == 0) return NULL;

  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  if (entry == NULL) return NULL;

  return bare_jni__wrapper(env, entry);
}

static void
bare_jni__on_token_finalize(js_env_t *env, void *data, void *finalize_hint) {
  int err;

  auto registry = static_cast<bare_jni_registry_t *>(finalize_hint);

  auto claim = static_cast<bare_jni__claim_t *>(data);

  uint32_t tag = claim->tag;

  err = js_delete_reference(env, claim->wrapper);
  assert(err == 0);

  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  if (entry->wrapper == claim->wrapper) entry->wrapper = NULL;

  delete claim;

  if (--entry->claims == 0) {
    JNIEnv *jni = bare_jni_env();

    jobject object = entry->object;

    auto range = registry->tags.equal_range(bare_jni__identity(jni, object));

    for (auto it = range.first; it != range.second; it++) {
      if (it->second != tag) continue;

      registry->tags.erase(it);
      break;
    }

    registry->entries.erase(tag);

    jni->DeleteGlobalRef(object);
  }

  bare_jni_registry_release(registry);
}

/**
 * Export as `claim(tag, wrapper)`. Return a token that keeps the object alive
 * until the token is garbage collected. A tag can be claimed more than once,
 * and the object stays alive until every token is gone. The registry does not
 * keep any wrapper alive.
 */
static js_value_t *
bare_jni_claim(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 2);

  uint32_t tag;
  err = bare_jni__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  if (entry == nullptr) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return NULL;
  }

  bare_jni__claim_t *claim = new bare_jni__claim_t();

  claim->tag = tag;

  err = js_create_reference(env, argv[1], 0, &claim->wrapper);
  assert(err == 0);

  if (bare_jni__wrapper(env, entry) == NULL) entry->wrapper = claim->wrapper;

  entry->claims++;

  bare_jni_registry_retain(registry);

  js_value_t *token;
  err = js_create_external(env, claim, bare_jni__on_token_finalize, registry, &token);
  assert(err == 0);

  return token;
}

/**
 * Export as `wrapper(tag)`. Return the first wrapper of `tag` that is still
 * alive, or `null` if there is none. An unknown tag is not an error, because
 * `adopt()` asks with a tag that may belong to another addon to find out
 * whether an object is ours.
 */
static js_value_t *
bare_jni_wrapper(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  uint32_t tag;
  err = bare_jni__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_jni_entry_t *entry = bare_jni__entry(registry, tag);

  js_value_t *result = entry == NULL ? NULL : bare_jni__wrapper(env, entry);

  if (result == NULL) {
    err = js_get_null(env, &result);
    assert(err == 0);
  }

  return result;
}

/**
 * Export as `registrySize()`. Return the number of objects in the registry.
 * Useful for finding leaks in tests.
 */
static js_value_t *
bare_jni_registry_size(js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &registry);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, registry->entries.size(), &result);
  assert(err == 0);

  return result;
}

static void
bare_jni__on_carrier_finalize(js_env_t *env, void *data, void *finalize_hint) {
  bare_jni_env()->DeleteGlobalRef(static_cast<jobject>(data));
}

/**
 * Export as `handle(tag)`. Return a handle that another addon can adopt. The
 * handle holds its own reference to the object.
 */
static js_value_t *
bare_jni_handle(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  jobject object;
  err = bare_jni_read_tag(env, registry, argv[0], "tag", &object);
  if (err < 0) return NULL;

  js_value_t *carrier;
  err = js_create_object(env, &carrier);
  assert(err == 0);

  err = js_wrap(env, carrier, bare_jni_env()->NewGlobalRef(object), bare_jni__on_carrier_finalize, NULL, NULL);
  assert(err == 0);

  err = js_add_type_tag(env, carrier, &bare_jni__carrier);
  assert(err == 0);

  return carrier;
}

/**
 * Export as `adopt(handle)`. Return a tag for the object in `handle`. A object
 * that already has a tag keeps it.
 */
static js_value_t *
bare_jni_adopt(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  bool is;
  err = js_is_object(env, argv[0], &is);
  assert(err == 0);

  if (is) {
    err = js_check_type_tag(env, argv[0], &bare_jni__carrier, &is);
    assert(err == 0);
  }

  if (!is) {
    err = js_throw_type_error(env, NULL, "Expected 'carrier' to be a Java handle");
    assert(err == 0);

    return NULL;
  }

  void *object;
  err = js_unwrap(env, argv[0], &object);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, bare_jni_tag(registry, static_cast<jobject>(object)), &result);
  assert(err == 0);

  return result;
}
