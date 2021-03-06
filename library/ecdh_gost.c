/*
 *  Elliptic curve GOST Diffie-Hellman
 *
 *  https://tools.ietf.org/html/rfc4357#page-7:
 *      VKO GOST R 34.10-2001 algorithm
 *
 *  http://tc26.ru/methods/recommendation/%D0%A2%D0%9A26%D0%90%D0%9B%D0%93.pdf page 9:
 *      VKO_GOSTR3410_2012_256 and VKO_GOSTR3410_2012_512 algorithms
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_ECDH_GOST_C)

#include "mbedtls/ecdh_gost.h"
#include "mbedtls/asn1write.h"

#include <string.h>

/* Implementation that should never be optimized out by the compiler */
static void mbedtls_zeroize( void *v, size_t n ) {
    volatile unsigned char *p = (unsigned char*)v; while( n-- ) *p++ = 0;
}

/*
 * Generate public key: simple wrapper around mbedtls_ecp_gen_keypair
 */
int mbedtls_ecdh_gost_gen_public( mbedtls_ecp_group *grp, mbedtls_mpi *d, mbedtls_ecp_point *Q,
                     int (*f_rng)(void *, unsigned char *, size_t),
                     void *p_rng )
{
    return mbedtls_ecp_gen_keypair( grp, d, Q, f_rng, p_rng );
}

/*
 * Compute shared secret
 */
int mbedtls_ecdh_gost_compute_shared( mbedtls_ecp_group *grp, unsigned char *z,
                         const mbedtls_ecp_point *Q, const mbedtls_mpi *d,
                         const unsigned char *ukm, size_t ukm_len, mbedtls_md_type_t md_alg,
                         int (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng )
{
    int ret;
    mbedtls_ecp_point P;
    mbedtls_mpi ukm_mpi;
    const mbedtls_md_info_t *md_info;
    mbedtls_md_context_t md_ctx;
    size_t n_size = ( grp->nbits + 7 ) / 8;
    unsigned char coord[MBEDTLS_ECP_MAX_BYTES];

    mbedtls_ecp_point_init( &P );
    mbedtls_mpi_init( &ukm_mpi );

    MBEDTLS_MPI_CHK( mbedtls_mpi_read_binary_le( &ukm_mpi, ukm, ukm_len ) );

    /*
     * Make sure Q is a valid pubkey before using it
     */
    MBEDTLS_MPI_CHK( mbedtls_ecp_check_pubkey( grp, Q ) );

    MBEDTLS_MPI_CHK( mbedtls_ecp_mul( grp, &P, d, Q, f_rng, p_rng ) );
    MBEDTLS_MPI_CHK( mbedtls_ecp_mul( grp, &P, &ukm_mpi, &P, f_rng, p_rng ) );

    if( mbedtls_ecp_is_zero( &P ) )
    {
        ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        goto cleanup;
    }

    if( ( md_info = mbedtls_md_info_from_type( md_alg ) ) == NULL )
            return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    mbedtls_md_init( &md_ctx );
    if( ( ret = mbedtls_md_setup( &md_ctx, md_info, 0 ) ) != 0 )
        return( ret );
    if( ( ret = mbedtls_md_starts( &md_ctx ) ) != 0 )
        return( ret );

    MBEDTLS_MPI_CHK( mbedtls_mpi_write_binary_le( &P.X, coord, n_size ) );
    if( ( ret = mbedtls_md_update( &md_ctx, coord, n_size ) ) != 0 )
        return( ret );


    MBEDTLS_MPI_CHK( mbedtls_mpi_write_binary_le( &P.Y, coord, n_size ) );
    if( ( ret = mbedtls_md_update( &md_ctx, coord, n_size ) ) != 0 )
        return( ret );

    if( ( ret = mbedtls_md_finish( &md_ctx, z ) ) != 0 )
        return( ret );

cleanup:
    mbedtls_ecp_point_free( &P );
    mbedtls_mpi_free( &ukm_mpi );
    mbedtls_md_free( &md_ctx );

    return( ret );
}

/*
 * Initialize context
 */
void mbedtls_ecdh_gost_init( mbedtls_ecdh_gost_context *ctx,
                             mbedtls_md_type_t gost_md_alg,
                             mbedtls_cipher_id_t gost89_alg )
{
    if( ctx == NULL )
        return;

    mbedtls_ecgost_init( &ctx->ecgost, gost_md_alg, gost89_alg, 1 );
    mbedtls_ecp_point_init( &ctx->Qp );
    memset( &ctx->z, 0, MBEDTLS_MD_MAX_SIZE );
}

/*
 * Free context
 */
void mbedtls_ecdh_gost_free( mbedtls_ecdh_gost_context *ctx )
{
    if( ctx == NULL )
        return;

    mbedtls_ecgost_free( &ctx->ecgost );
    mbedtls_ecp_point_free( &ctx->Qp );
    mbedtls_zeroize( &ctx->z, MBEDTLS_MD_MAX_SIZE );
}

/*
 * Get parameters from a keypair
 */
int mbedtls_ecdh_gost_get_params( mbedtls_ecdh_gost_context *ctx, const mbedtls_ecgost_context *key,
                     mbedtls_ecdh_gost_side side )
{
    int ret;

    ctx->ecgost.gost_md_alg = key->gost_md_alg;
    ctx->ecgost.gost89_alg = key->gost89_alg;

    if( ( ret = mbedtls_ecp_group_copy( &ctx->ecgost.key.grp, &key->key.grp ) ) != 0 )
        return( ret );

    /* If it's not our key, just import the public part as Qp */
    if( side == MBEDTLS_ECDH_GOST_THEIRS )
        return( mbedtls_ecp_copy( &ctx->Qp, &key->key.Q ) );

    /* Our key: import public (as Q) and private parts */
    if( side != MBEDTLS_ECDH_GOST_OURS )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( ret = mbedtls_ecp_copy( &ctx->ecgost.key.Q, &key->key.Q ) ) != 0 ||
        ( ret = mbedtls_mpi_copy( &ctx->ecgost.key.d, &key->key.d ) ) != 0 )
        return( ret );

    return( 0 );
}

/*
 * Setup and export the client public value
 */
int mbedtls_ecdh_gost_make_public( mbedtls_ecdh_gost_context *ctx,
                      const mbedtls_pk_info_t *pk_info,
                      size_t *olen, unsigned char *buf, size_t blen,
                      int (*f_rng)(void *, unsigned char *, size_t),
                      void *p_rng )
{
    int ret;
    mbedtls_pk_context pk;
    unsigned char pubkey[200];
    size_t pub_len = 0;

    if( ctx == NULL || ctx->ecgost.key.grp.pbits == 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( ret = mbedtls_ecdh_gost_gen_public( &ctx->ecgost.key.grp, &ctx->ecgost.key.d,
                        &ctx->ecgost.key.Q, f_rng, p_rng ) ) != 0 )
        return( ret );

    /* Clearly set key_exchange to 1 because mbedtls_ecdh_gost_init is not called */
    ctx->ecgost.key_exchange = 1;

    pk.pk_ctx = &ctx->ecgost;
    pk.pk_info = pk_info;

    MBEDTLS_ASN1_CHK_ADD( pub_len, mbedtls_pk_write_pubkey_der( &pk, pubkey, sizeof( pubkey ) ) );

    *olen = pub_len;
    if( blen < pub_len )
        return( MBEDTLS_ERR_ECP_BUFFER_TOO_SMALL );

    memcpy( buf, pubkey + sizeof( pubkey ) - pub_len, pub_len );

    return( 0 );
}

/*
 * Parse and import the client's public value
 */
int mbedtls_ecdh_gost_read_public( mbedtls_ecdh_gost_context *ctx,
                      const mbedtls_pk_info_t *pk_info,
                      const unsigned char *buf, size_t blen )
{
    int ret;
    mbedtls_pk_context pk;
    mbedtls_ecgost_context dummy_pub_key;

    if( ctx == NULL )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    /* Just set key_exchange flag without full initialization */
    dummy_pub_key.key_exchange = 1;

    pk.pk_ctx = &dummy_pub_key;
    pk.pk_info = pk_info;

    /* Allocates memory in pk, need to call mbedtls_pk_free */
    if( ( ret = mbedtls_pk_parse_public_key( &pk, buf, blen ) ) != 0 )
        goto cleanup;

    ret = mbedtls_ecp_copy( &ctx->Qp, &mbedtls_pk_ecgost( pk )->key.Q );

cleanup:
    mbedtls_pk_free( &pk );

    return( ret );
}

/*
 * Derive and export the shared secret
 */
int mbedtls_ecdh_gost_calc_secret( mbedtls_ecdh_gost_context *ctx,
                      mbedtls_md_type_t md_alg,
                      const unsigned char *ukm, size_t ukm_len,
                      size_t *olen, unsigned char *buf, size_t blen,
                      int (*f_rng)(void *, unsigned char *, size_t),
                      void *p_rng )
{
    int ret;
    const mbedtls_md_info_t *md_info;
    size_t hash_len;

    if( ctx == NULL )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( md_info = mbedtls_md_info_from_type( md_alg ) ) == NULL )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    hash_len = mbedtls_md_get_size( md_info );

    if( hash_len > blen )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( ret = mbedtls_ecdh_gost_compute_shared( &ctx->ecgost.key.grp, ctx->z, &ctx->Qp,
                                     &ctx->ecgost.key.d, ukm, ukm_len, md_alg,
                                     f_rng, p_rng ) ) != 0 )
    {
        return( ret );
    }

    memcpy( buf, ctx->z, hash_len );
    *olen = hash_len;

    return( ret );
}

#endif /* MBEDTLS_ECDH_GOST_C */
