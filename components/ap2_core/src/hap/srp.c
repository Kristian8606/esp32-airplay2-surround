#include "srp.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/bignum.h"
#include "sodium.h"

static const char *TAG = "srp";

#define AP2_SRP_EPHEMERAL_BYTES 32
#define AP2_SRP_GENERATOR       5

/* RFC 5054 appendix A, 3072-bit group. */
static const uint8_t group_prime[AP2_SRP_PRIME_BYTES] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED,
    0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5, 0xAE, 0x9F, 0x24, 0x11,
    0x7C, 0x4B, 0x1F, 0xE6, 0x49, 0x28, 0x66, 0x51, 0xEC, 0xE4, 0x5B, 0x3D,
    0xC2, 0x00, 0x7C, 0xB8, 0xA1, 0x63, 0xBF, 0x05, 0x98, 0xDA, 0x48, 0x36,
    0x1C, 0x55, 0xD3, 0x9A, 0x69, 0x16, 0x3F, 0xA8, 0xFD, 0x24, 0xCF, 0x5F,
    0x83, 0x65, 0x5D, 0x23, 0xDC, 0xA3, 0xAD, 0x96, 0x1C, 0x62, 0xF3, 0x56,
    0x20, 0x85, 0x52, 0xBB, 0x9E, 0xD5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6D,
    0x67, 0x0C, 0x35, 0x4E, 0x4A, 0xBC, 0x98, 0x04, 0xF1, 0x74, 0x6C, 0x08,
    0xCA, 0x18, 0x21, 0x7C, 0x32, 0x90, 0x5E, 0x46, 0x2E, 0x36, 0xCE, 0x3B,
    0xE3, 0x9E, 0x77, 0x2C, 0x18, 0x0E, 0x86, 0x03, 0x9B, 0x27, 0x83, 0xA2,
    0xEC, 0x07, 0xA2, 0x8F, 0xB5, 0xC5, 0x5D, 0xF0, 0x6F, 0x4C, 0x52, 0xC9,
    0xDE, 0x2B, 0xCB, 0xF6, 0x95, 0x58, 0x17, 0x18, 0x39, 0x95, 0x49, 0x7C,
    0xEA, 0x95, 0x6A, 0xE5, 0x15, 0xD2, 0x26, 0x18, 0x98, 0xFA, 0x05, 0x10,
    0x15, 0x72, 0x8E, 0x5A, 0x8A, 0xAA, 0xC4, 0x2D, 0xAD, 0x33, 0x17, 0x0D,
    0x04, 0x50, 0x7A, 0x33, 0xA8, 0x55, 0x21, 0xAB, 0xDF, 0x1C, 0xBA, 0x64,
    0xEC, 0xFB, 0x85, 0x04, 0x58, 0xDB, 0xEF, 0x0A, 0x8A, 0xEA, 0x71, 0x57,
    0x5D, 0x06, 0x0C, 0x7D, 0xB3, 0x97, 0x0F, 0x85, 0xA6, 0xE1, 0xE4, 0xC7,
    0xAB, 0xF5, 0xAE, 0x8C, 0xDB, 0x09, 0x33, 0xD7, 0x1E, 0x8C, 0x94, 0xE0,
    0x4A, 0x25, 0x61, 0x9D, 0xCE, 0xE3, 0xD2, 0x26, 0x1A, 0xD2, 0xEE, 0x6B,
    0xF1, 0x2F, 0xFA, 0x06, 0xD9, 0x8A, 0x08, 0x64, 0xD8, 0x76, 0x02, 0x73,
    0x3E, 0xC8, 0x6A, 0x64, 0x52, 0x1F, 0x2B, 0x18, 0x17, 0x7B, 0x20, 0x0C,
    0xBB, 0xE1, 0x17, 0x57, 0x7A, 0x61, 0x5D, 0x6C, 0x77, 0x09, 0x88, 0xC0,
    0xBA, 0xD9, 0x46, 0xE2, 0x08, 0xE2, 0x4F, 0xA0, 0x74, 0xE5, 0xAB, 0x31,
    0x43, 0xDB, 0x5B, 0xFC, 0xE0, 0xFD, 0x10, 0x8E, 0x4B, 0x82, 0xD1, 0x20,
    0xA9, 0x3A, 0xD2, 0xCA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

struct ap2_srp {
  uint8_t salt[AP2_SRP_SALT_BYTES];
  uint8_t ephemeral[AP2_SRP_EPHEMERAL_BYTES]; /* b */
  uint8_t verifier[AP2_SRP_PRIME_BYTES];      /* v */
  uint8_t pub_b[AP2_SRP_PRIME_BYTES];         /* B, padded */
  uint8_t pub_a[AP2_SRP_PRIME_BYTES];         /* A, padded */
  uint8_t premaster[AP2_SRP_PRIME_BYTES];     /* S, padded */
  uint8_t user_hash[crypto_hash_sha512_BYTES];
  uint8_t session_key[AP2_SRP_SESSION_KEY_BYTES];
  uint8_t server_proof[AP2_SRP_PROOF_BYTES];
  bool started;
  bool verified;
};

ap2_srp_t *ap2_srp_new(void) {
  return calloc(1, sizeof(ap2_srp_t));
}

void ap2_srp_delete(ap2_srp_t *session) {
  if (session) {
    sodium_memzero(session, sizeof(*session));
    free(session);
  }
}

static int mpi_from_u32(mbedtls_mpi *x, int value) {
  return mbedtls_mpi_lset(x, value);
}

/* k = H(N | PAD(g)) */
static void multiplier_hash(uint8_t out[crypto_hash_sha512_BYTES]) {
  uint8_t padded_g[AP2_SRP_PRIME_BYTES] = {0};
  crypto_hash_sha512_state st;

  padded_g[AP2_SRP_PRIME_BYTES - 1] = AP2_SRP_GENERATOR;
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, group_prime, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512_update(&st, padded_g, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512_final(&st, out);
}

esp_err_t ap2_srp_start(ap2_srp_t *session, const char *username,
                        const char *password) {
  mbedtls_mpi n, g, x, v, b, k, gb, pub;
  uint8_t digest[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;
  esp_err_t result = ESP_FAIL;
  int rc;

  if (!session || !username || !password) {
    return ESP_ERR_INVALID_ARG;
  }
  session->started = false;
  session->verified = false;

  esp_fill_random(session->salt, sizeof(session->salt));
  esp_fill_random(session->ephemeral, sizeof(session->ephemeral));

  /* H(I) goes into the proofs later. */
  crypto_hash_sha512(session->user_hash, (const uint8_t *)username,
                     strlen(username));

  /* x = H(s | H(I ":" P)) */
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, (const uint8_t *)username, strlen(username));
  crypto_hash_sha512_update(&st, (const uint8_t *)":", 1);
  crypto_hash_sha512_update(&st, (const uint8_t *)password, strlen(password));
  crypto_hash_sha512_final(&st, digest);
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, session->salt, sizeof(session->salt));
  crypto_hash_sha512_update(&st, digest, sizeof(digest));
  crypto_hash_sha512_final(&st, digest);

  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&v);
  mbedtls_mpi_init(&b);
  mbedtls_mpi_init(&k);
  mbedtls_mpi_init(&gb);
  mbedtls_mpi_init(&pub);

#define MPI_DO(call)                                                           \
  do {                                                                         \
    rc = (call);                                                               \
    if (rc != 0) {                                                             \
      goto done;                                                               \
    }                                                                          \
  } while (0)

  MPI_DO(mbedtls_mpi_read_binary(&n, group_prime, AP2_SRP_PRIME_BYTES));
  MPI_DO(mpi_from_u32(&g, AP2_SRP_GENERATOR));
  MPI_DO(mbedtls_mpi_read_binary(&x, digest, sizeof(digest)));
  /* v = g^x */
  MPI_DO(mbedtls_mpi_exp_mod(&v, &g, &x, &n, NULL));
  MPI_DO(mbedtls_mpi_write_binary(&v, session->verifier, AP2_SRP_PRIME_BYTES));

  /* B = k*v + g^b */
  multiplier_hash(digest);
  MPI_DO(mbedtls_mpi_read_binary(&k, digest, sizeof(digest)));
  MPI_DO(mbedtls_mpi_read_binary(&b, session->ephemeral, AP2_SRP_EPHEMERAL_BYTES));
  MPI_DO(mbedtls_mpi_exp_mod(&gb, &g, &b, &n, NULL));
  MPI_DO(mbedtls_mpi_mul_mpi(&pub, &k, &v));
  MPI_DO(mbedtls_mpi_add_mpi(&pub, &pub, &gb));
  MPI_DO(mbedtls_mpi_mod_mpi(&pub, &pub, &n));
  MPI_DO(mbedtls_mpi_write_binary(&pub, session->pub_b, AP2_SRP_PRIME_BYTES));

  session->started = true;
  result = ESP_OK;

done:
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "big number error %d", rc);
  }
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&g);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&b);
  mbedtls_mpi_free(&k);
  mbedtls_mpi_free(&gb);
  mbedtls_mpi_free(&pub);
  sodium_memzero(digest, sizeof(digest));
  return result;
}

const uint8_t *ap2_srp_salt(ap2_srp_t *session) {
  return session->salt;
}

const uint8_t *ap2_srp_public_key(ap2_srp_t *session, size_t *len) {
  if (len) {
    *len = AP2_SRP_PRIME_BYTES;
  }
  return session->pub_b;
}

static size_t leading_zeros(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len && data[i] == 0) {
    i++;
  }
  return i;
}

/* M1 = H(H(N) xor H(g) | H(I) | s | A | B | K), M2 = H(A | M1 | K).
 * pad_ab chooses whether A and B keep their leading zero bytes here. */
static void compute_proofs(const ap2_srp_t *s, const uint8_t key[64],
                           bool pad_ab, uint8_t m1[64], uint8_t m2[64]) {
  uint8_t mix[crypto_hash_sha512_BYTES], hash_g[crypto_hash_sha512_BYTES];
  const uint8_t g = AP2_SRP_GENERATOR;
  size_t skip_a = pad_ab ? 0 : leading_zeros(s->pub_a, AP2_SRP_PRIME_BYTES);
  size_t skip_b = pad_ab ? 0 : leading_zeros(s->pub_b, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512_state st;

  crypto_hash_sha512(mix, group_prime, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512(hash_g, &g, 1);
  for (size_t i = 0; i < sizeof(mix); i++) {
    mix[i] ^= hash_g[i];
  }

  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, mix, sizeof(mix));
  crypto_hash_sha512_update(&st, s->user_hash, sizeof(s->user_hash));
  crypto_hash_sha512_update(&st, s->salt, sizeof(s->salt));
  crypto_hash_sha512_update(&st, s->pub_a + skip_a, AP2_SRP_PRIME_BYTES - skip_a);
  crypto_hash_sha512_update(&st, s->pub_b + skip_b, AP2_SRP_PRIME_BYTES - skip_b);
  crypto_hash_sha512_update(&st, key, AP2_SRP_SESSION_KEY_BYTES);
  crypto_hash_sha512_final(&st, m1);

  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, s->pub_a + skip_a, AP2_SRP_PRIME_BYTES - skip_a);
  crypto_hash_sha512_update(&st, m1, AP2_SRP_PROOF_BYTES);
  crypto_hash_sha512_update(&st, key, AP2_SRP_SESSION_KEY_BYTES);
  crypto_hash_sha512_final(&st, m2);
}

/* S = (A * v^u)^b mod N into session->premaster. Fails if A is 0 mod N or u
 * is 0, the two values SRP-6a must refuse. */
static esp_err_t compute_premaster(ap2_srp_t *s) {
  mbedtls_mpi n, a, v, u, b, tmp, premaster;
  uint8_t digest[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;
  esp_err_t result = ESP_FAIL;
  int rc;

  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, s->pub_a, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512_update(&st, s->pub_b, AP2_SRP_PRIME_BYTES);
  crypto_hash_sha512_final(&st, digest);

  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&a);
  mbedtls_mpi_init(&v);
  mbedtls_mpi_init(&u);
  mbedtls_mpi_init(&b);
  mbedtls_mpi_init(&tmp);
  mbedtls_mpi_init(&premaster);

  MPI_DO(mbedtls_mpi_read_binary(&n, group_prime, AP2_SRP_PRIME_BYTES));
  MPI_DO(mbedtls_mpi_read_binary(&a, s->pub_a, AP2_SRP_PRIME_BYTES));
  MPI_DO(mbedtls_mpi_mod_mpi(&a, &a, &n));
  MPI_DO(mbedtls_mpi_read_binary(&u, digest, sizeof(digest)));
  if (mbedtls_mpi_cmp_int(&a, 0) == 0 || mbedtls_mpi_cmp_int(&u, 0) == 0) {
    rc = 0;
    goto done;
  }
  MPI_DO(mbedtls_mpi_read_binary(&v, s->verifier, AP2_SRP_PRIME_BYTES));
  MPI_DO(mbedtls_mpi_read_binary(&b, s->ephemeral, AP2_SRP_EPHEMERAL_BYTES));
  MPI_DO(mbedtls_mpi_exp_mod(&tmp, &v, &u, &n, NULL));
  MPI_DO(mbedtls_mpi_mul_mpi(&tmp, &a, &tmp));
  MPI_DO(mbedtls_mpi_mod_mpi(&tmp, &tmp, &n));
  MPI_DO(mbedtls_mpi_exp_mod(&premaster, &tmp, &b, &n, NULL));
  MPI_DO(mbedtls_mpi_write_binary(&premaster, s->premaster, AP2_SRP_PRIME_BYTES));
  result = ESP_OK;

done:
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&a);
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&u);
  mbedtls_mpi_free(&b);
  mbedtls_mpi_free(&tmp);
  mbedtls_mpi_free(&premaster);
  sodium_memzero(digest, sizeof(digest));
  return result;
}

esp_err_t ap2_srp_verify_client(ap2_srp_t *session,
                                const uint8_t *client_public_key,
                                size_t client_pk_len, const uint8_t *client_proof,
                                size_t proof_len) {
  if (!session || !client_public_key || !client_proof) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->started) {
    return ESP_ERR_INVALID_STATE;
  }
  if (client_pk_len == 0 || client_pk_len > AP2_SRP_PRIME_BYTES ||
      proof_len != AP2_SRP_PROOF_BYTES) {
    return ESP_ERR_INVALID_SIZE;
  }

  session->verified = false;
  memset(session->pub_a, 0, AP2_SRP_PRIME_BYTES);
  memcpy(session->pub_a + AP2_SRP_PRIME_BYTES - client_pk_len, client_public_key,
         client_pk_len);

  esp_err_t err = compute_premaster(session);
  if (err != ESP_OK) {
    return err;
  }

  /* Implementations differ on whether leading zero bytes of S, A and B are
   * hashed, which only shows about once in 256 pairings. The client's proof
   * tells which it used: try the forms that differ for these values. */
  size_t zeros_s = leading_zeros(session->premaster, AP2_SRP_PRIME_BYTES);
  bool ab_differ = session->pub_a[0] == 0 || session->pub_b[0] == 0;
  uint8_t key[AP2_SRP_SESSION_KEY_BYTES], m1[AP2_SRP_PROOF_BYTES], m2[AP2_SRP_PROOF_BYTES];

  err = ESP_FAIL;
  for (int pad_s = 1; pad_s >= (zeros_s ? 0 : 1) && err != ESP_OK; pad_s--) {
    size_t skip = pad_s ? 0 : zeros_s;
    crypto_hash_sha512(key, session->premaster + skip, AP2_SRP_PRIME_BYTES - skip);
    for (int pad_ab = 0; pad_ab <= (ab_differ ? 1 : 0); pad_ab++) {
      compute_proofs(session, key, pad_ab != 0, m1, m2);
      if (sodium_memcmp(m1, client_proof, AP2_SRP_PROOF_BYTES) == 0) {
        memcpy(session->session_key, key, sizeof(key));
        memcpy(session->server_proof, m2, sizeof(m2));
        session->verified = true;
        err = ESP_OK;
        break;
      }
    }
  }

  sodium_memzero(session->premaster, sizeof(session->premaster));
  sodium_memzero(key, sizeof(key));
  sodium_memzero(m1, sizeof(m1));
  sodium_memzero(m2, sizeof(m2));
  return err;
}

const uint8_t *ap2_srp_proof(ap2_srp_t *session) {
  return session->server_proof;
}

const uint8_t *ap2_srp_session_key(ap2_srp_t *session, size_t *len) {
  if (!session || !session->verified) {
    if (len) {
      *len = 0;
    }
    return NULL;
  }
  if (len) {
    *len = AP2_SRP_SESSION_KEY_BYTES;
  }
  return session->session_key;
}
