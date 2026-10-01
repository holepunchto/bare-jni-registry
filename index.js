const tag = (exports.tag = Symbol.for('bare.jni.tag'))
const handle = (exports.handle = Symbol.for('bare.jni.handle'))

const adopted = Symbol('bare.jni.adopted')

exports.adopt = function adopt(binding, object) {
  if (object === null || object === undefined) return null

  // Objects from other addons have a tag too, but it means something else
  // there. The tag is only ours if our registry maps it back to this object.
  const existingTag = object[tag]

  if (typeof existingTag === 'number' && binding.wrapper(existingTag) === object) return existingTag

  const existing = object[adopted]

  if (existing !== undefined && existing.binding === binding) return existing.tag

  const carrier = object[handle]

  if (carrier === undefined) {
    throw new TypeError('Object does not implement the Java handle protocol')
  }

  const adoptedTag = binding.adopt(carrier)

  // Claiming keeps the object alive for as long as the foreign wrapper, and
  // makes our addon hand back that wrapper rather than making a new one.
  const token = binding.claim(adoptedTag, object)

  Object.defineProperty(object, adopted, {
    value: { binding, tag: adoptedTag, token },
    configurable: true
  })

  return adoptedTag
}
