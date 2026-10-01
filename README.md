# bare-jni-registry

Keep track of the Java objects that a Bare addon gives to JavaScript, and pass them from one addon to another.

An addon does not give JavaScript a pointer to an object. It gives it a number, called a tag. The registry remembers which object each tag stands for and which JavaScript object wraps it, and it keeps the object alive for as long as that wrapper is alive.

Every addon has its own registry, so a tag only means something to the addon that made it. To pass an object to another addon, you turn it into a handle, and the other addon adopts the handle into its own registry.

The module has two parts: `registry.h` for the native code of an addon, and a JavaScript module for its JavaScript code.

```
npm i bare-jni-registry
```

## Usage

### Setting up an addon

Add the header to the include path of the addon in `CMakeLists.txt`:

```cmake
target_include_directories(
  ${my_addon}
  PRIVATE
    node_modules/bare-jni-registry/lib
)
```

Before you create a registry, call `bare_jni_attach()` once with a `JNIEnv`. The registry needs the Java VM to keep track of objects, and JNI does not offer one on its own.

Create a registry when the addon loads, and pass it as the data pointer of every function the addon exports. Export the five functions of the registry next to your own:

```cpp
#include "registry.h"

static js_value_t *
my_addon_exports(js_env_t *env, js_value_t *exports) {
  int err;

  bare_jni_registry_t *registry = bare_jni_registry_create(env, exports);

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, registry, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("claim", bare_jni_claim)
  V("wrapper", bare_jni_wrapper)
  V("handle", bare_jni_handle)
  V("adopt", bare_jni_adopt)
  V("registrySize", bare_jni_registry_size)

  // Your own functions go here

#undef V

  return exports;
}
```

### Giving Java objects to JavaScript

Your functions get the registry back from their data pointer. Give an object a tag before you return it, and turn a tag back into an object when JavaScript passes one in:

```cpp
#include <jni.h>
#include <js.h>

#include "registry.h"

static jclass my_addon_view_class;

static js_value_t *
my_addon_view_set_alpha(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_jni_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  jobject view;
  err = bare_jni_read_type(env, registry, argv[0], "view", my_addon_view_class, &view);
  if (err < 0) return NULL;

  // Read the alpha from argv[1] and call View.setAlpha() through bare_jni_env()

  return NULL;
}
```

### Wrapping Java objects in JavaScript

On the JavaScript side, wrap each tag in an object and claim it. Claiming tells the registry which object wraps the tag, and returns a token that keeps the object alive until the wrapper is garbage collected.

The wrapper gives its tag to the registry through the `registry.tag` symbol. Where it keeps the tag is up to you:

```js
const binding = require('./binding')
const registry = require('bare-jni-registry')

class Label {
  constructor(tag) {
    this._tag = tag
    this._token = binding.claim(tag, this)
  }

  get [registry.tag]() {
    return this._tag
  }
}
```

When the addon returns a tag that JavaScript has seen before, reuse the wrapper it already has:

```js
function wrap(tag) {
  if (tag === null) return null

  return binding.wrapper(tag) || new Label(tag)
}
```

### Passing Java objects between addons

A wrapper that other addons may use also gives out a handle through the `registry.handle` symbol. A handle carries the object itself rather than a tag, so another addon can take it into its own registry:

```js
class Label {
  // ...

  get [registry.handle]() {
    return binding.handle(this._tag)
  }
}
```

An addon that takes an object from JavaScript adopts it before it passes it to native code. Adopting returns a tag in its own registry, whichever addon the object came from:

```js
const binding = require('./binding')
const registry = require('bare-jni-registry')

class Box {
  set child(child) {
    binding.boxSetChild(this._tag, registry.adopt(binding, child))
  }
}
```

## License

Apache-2.0
