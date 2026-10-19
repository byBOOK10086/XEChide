# XEChide manager does not enable R8/minification in the release build.
# These rules are kept minimal so enabling minification later is predictable.
#
# No reflection is used for the config format (ByteBuffer) or for persistence
# (org.json is a framework class). The entry points are declared in the
# manifest, so AGP's default keep rules cover them.

-keepattributes SourceFile,LineNumberTable
