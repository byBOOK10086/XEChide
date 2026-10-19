package dev.xechide.manager.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Encrypted persistence for [ManagerConfig].
 *
 * The JSON is encrypted with AES-256-GCM using a key that never leaves the
 * Android Keystore. Only the IV and ciphertext are stored (base64) in the
 * app-private SharedPreferences file. A missing, invalidated or undecryptable
 * key simply makes [load] return null: the caller can then continue with an
 * empty (fail-open) configuration.
 */
class SecureStore(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences(
        PREFERENCES_NAME,
        Context.MODE_PRIVATE,
    )

    @Synchronized
    fun save(config: ManagerConfig) {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
        val plaintext = ConfigCodec.encodeJson(config).toByteArray(Charsets.UTF_8)
        val ciphertext = cipher.doFinal(plaintext)
        val committed = preferences.edit()
            .putString(KEY_IV, Base64.encodeToString(cipher.iv, Base64.NO_WRAP))
            .putString(KEY_CIPHERTEXT, Base64.encodeToString(ciphertext, Base64.NO_WRAP))
            .commit()
        if (!committed) {
            throw IllegalStateException("could not persist the encrypted configuration")
        }
    }

    @Synchronized
    fun load(): ManagerConfig? {
        val encodedIv = preferences.getString(KEY_IV, null) ?: return null
        val encodedCiphertext = preferences.getString(KEY_CIPHERTEXT, null) ?: return null
        return try {
            val iv = Base64.decode(encodedIv, Base64.NO_WRAP)
            val ciphertext = Base64.decode(encodedCiphertext, Base64.NO_WRAP)
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), GCMParameterSpec(128, iv))
            val plaintext = cipher.doFinal(ciphertext)
            ConfigCodec.decodeJson(plaintext.toString(Charsets.UTF_8))
        } catch (error: Exception) {
            // Fail open. Never fall back to plaintext or to a stale in-memory
            // list: the kernel starts each boot with zero rules by design.
            Log.w(TAG, "stored configuration is unavailable", error)
            null
        }
    }

    private fun getOrCreateKey(): SecretKey = synchronized(KEY_LOCK) {
        val keyStore = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        val existing = keyStore.getEntry(KEY_ALIAS, null)
        if (existing is KeyStore.SecretKeyEntry) {
            return@synchronized existing.secretKey
        }

        val generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES,
            ANDROID_KEYSTORE,
        )
        val specification = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build()
        generator.init(specification)
        generator.generateKey()
    }

    private companion object {
        const val TAG = "XeChideSecureStore"
        const val ANDROID_KEYSTORE = "AndroidKeyStore"
        const val KEY_ALIAS = "dev.xechide.manager.config.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val PREFERENCES_NAME = "xechide_config"
        const val KEY_IV = "iv"
        const val KEY_CIPHERTEXT = "ciphertext"
        val KEY_LOCK = Any()
    }
}
