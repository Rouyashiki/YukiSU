package org.rouyashiki.yukisu.update

import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ksuApp
import org.bouncycastle.bcpg.EdDSAPublicBCPGKey
import org.bouncycastle.bcpg.HashAlgorithmTags
import org.bouncycastle.bcpg.PublicKeyAlgorithmTags
import org.bouncycastle.crypto.digests.SHA512Digest
import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer
import org.bouncycastle.openpgp.PGPException
import org.bouncycastle.openpgp.PGPPublicKey
import org.bouncycastle.openpgp.operator.PGPContentVerifier
import org.bouncycastle.openpgp.operator.PGPContentVerifierBuilder
import org.bouncycastle.openpgp.operator.PGPContentVerifierBuilderProvider
import java.io.OutputStream

/** Verifies only the OpenPGP algorithm pair used by the pinned CI signing key. */
internal object Ed25519PgpContentVerifierProvider : PGPContentVerifierBuilderProvider {
    private const val LEGACY_ED25519_CURVE_OID = "1.3.6.1.4.1.11591.15.1"

    override fun get(keyAlgorithm: Int, hashAlgorithm: Int): PGPContentVerifierBuilder {
        if (keyAlgorithm != PublicKeyAlgorithmTags.EDDSA_LEGACY) {
            throw PGPException(ksuApp.getString(R.string.ci_update_error_signature_invalid))
        }
        if (hashAlgorithm != HashAlgorithmTags.SHA512) {
            throw PGPException(ksuApp.getString(R.string.ci_update_error_signature_invalid))
        }
        return PGPContentVerifierBuilder { publicKey -> buildVerifier(publicKey) }
    }

    private fun buildVerifier(publicKey: PGPPublicKey): PGPContentVerifier {
        if (publicKey.algorithm != PublicKeyAlgorithmTags.EDDSA_LEGACY) {
            throw PGPException(ksuApp.getString(R.string.ci_update_error_signature_invalid))
        }
        val key = publicKey.publicKeyPacket.key as? EdDSAPublicBCPGKey
            ?: throw PGPException(
                ksuApp.getString(R.string.ci_update_error_signature_invalid)
            )
        if (key.curveOID.id != LEGACY_ED25519_CURVE_OID) {
            throw PGPException(ksuApp.getString(R.string.ci_update_error_signature_invalid))
        }

        val encodedPoint = key.encodedPoint.toByteArray()
        if (encodedPoint.size != 33 || encodedPoint[0] != 0x40.toByte()) {
            throw PGPException(ksuApp.getString(R.string.ci_update_error_signature_invalid))
        }
        return Ed25519PgpContentVerifier(
            publicKey.keyID,
            Ed25519PublicKeyParameters(encodedPoint, 1),
        )
    }
}

private class Ed25519PgpContentVerifier(
    private val keyId: Long,
    publicKey: Ed25519PublicKeyParameters,
) : PGPContentVerifier {
    private val digest = SHA512Digest()
    private val signer = Ed25519Signer().apply { init(false, publicKey) }
    private val output = object : OutputStream() {
        override fun write(value: Int) = digest.update(value.toByte())

        override fun write(buffer: ByteArray, offset: Int, length: Int) =
            digest.update(buffer, offset, length)
    }

    override fun getOutputStream(): OutputStream = output

    override fun getHashAlgorithm(): Int = HashAlgorithmTags.SHA512

    override fun getKeyAlgorithm(): Int = PublicKeyAlgorithmTags.EDDSA_LEGACY

    override fun getKeyID(): Long = keyId

    override fun verify(expected: ByteArray): Boolean {
        val documentHash = ByteArray(digest.digestSize)
        digest.doFinal(documentHash, 0)
        signer.update(documentHash, 0, documentHash.size)
        return signer.verifySignature(expected)
    }
}
