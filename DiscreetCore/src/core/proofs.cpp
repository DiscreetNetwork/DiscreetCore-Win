#include <cstdlib>
//#include <mutex>
//#include <thread>

extern "C" {
#include "crypto_curve.h"
}

#include "proofs.h"
#include "multiexp.h"
#include "ops.h"
#include "types.h"

#include "util/varint.h"
#include "util/memwipe.h"

namespace discore {
    /* FOR US, m = 2, n >= 6 */
    static const size_t max_mn = 64;
    static const size_t min_mn = 64;

    static std::shared_ptr<pippenger_cache> cache;
    static ge_p3 Hi_p3[max_mn];
    static ge_p3 H_p3;
    static ge_p3 G_p3;
    static key U;
    static ge_p3 U_p3;
#if !defined(_WIN32)
    static std::mutex init_mutex;
#endif
    static const key ZERO = zero();
    static const key ONE = identity();
    static const key IDENTITY = identity();
    static const key TWO = { {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00} };
    static const key MINUS_ONE = { {0xec, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10} };

    static void transcript_init(key &transcript)
    {
        std::string salt("triptych transcript");
        hash_to_scalar(transcript, salt.data(), salt.size());
    }

    static void transcript_update_mu(key &transcript, const key &message, const keyV &M, const keyV &P, const key &C_offset, const key &J, const key &K, const key &A, const key &B, const key &C, const key &D)
    {
        CHECK_THROW_ERR(M.size() == P.size(), "Transcript challenge inputs have incorrect size!");

        std::string hash;
        hash.reserve((2*M.size() + 9)*sizeof(key));

        hash = std::string((const char*) transcript.bytes, sizeof(transcript));
        hash += std::string((const char*) message.bytes, sizeof(message));

        for (size_t k = 0; k < M.size(); k++)
        {
            hash += std::string((const char*) M[k].bytes, sizeof(M[k]));
            hash += std::string((const char*) P[k].bytes, sizeof(P[k]));
        }

        hash += std::string((const char*) C_offset.bytes, sizeof(C_offset));
        hash += std::string((const char*) J.bytes, sizeof(J));
        hash += std::string((const char*) K.bytes, sizeof(K));
        hash += std::string((const char*) A.bytes, sizeof(A));
        hash += std::string((const char*) B.bytes, sizeof(B));
        hash += std::string((const char*) C.bytes, sizeof(C));
        hash += std::string((const char*) D.bytes, sizeof(D));

        CHECK_THROW_ERR(hash.size() > 1, "Bad hash input size!");
        hash_to_scalar(transcript, hash.data(), hash.size());

        CHECK_THROW_ERR(!(transcript == ZERO), "Transcript challenge must be nonzero!");
    }

    static void transcript_update_x(key &transcript, const keyV &X, const keyV &Y)
    {
        CHECK_THROW_ERR(X.size() == Y.size(), "Transcript challenge inputs have incorrect size!");

        std::string hash;
        hash.reserve((2*X.size() + 1)*sizeof(key));

        hash = std::string((const char*) transcript.bytes, sizeof(transcript));
        
        for (size_t j = 0; j < X.size(); j++)
        {
            hash += std::string((const char*) X[j].bytes, sizeof(X[j]));
            hash += std::string((const char*) Y[j].bytes, sizeof(Y[j]));
        }

        CHECK_THROW_ERR(hash.size() > 1, "Bad hash input size!");
        hash_to_scalar(transcript,hash.data(),hash.size());

        CHECK_THROW_ERR(!(transcript == ZERO), "Transcript challenge must be nonzero!");
    }

    static key sm(key y, int n, const key &x)
    {
        while (n--) {
            sc_mul(y.bytes, y.bytes, y.bytes);
        }

        sc_mul(y.bytes, y.bytes, x.bytes);

        return y;
    }

    static key invert(const key &x)
    {
        key _1, _10, _100, _11, _101, _111, _1001, _1011, _1111;

        _1 = x;
        sc_mul(_10.bytes, _1.bytes, _1.bytes);
        sc_mul(_100.bytes, _10.bytes, _10.bytes);
        sc_mul(_11.bytes, _10.bytes, _1.bytes);
        sc_mul(_101.bytes, _10.bytes, _11.bytes);
        sc_mul(_111.bytes, _10.bytes, _101.bytes);
        sc_mul(_1001.bytes, _10.bytes, _111.bytes);
        sc_mul(_1011.bytes, _10.bytes, _1001.bytes);
        sc_mul(_1111.bytes, _100.bytes, _1011.bytes);

        key inv;
        sc_mul(inv.bytes, _1111.bytes, _1.bytes);

        inv = sm(inv, 123 + 3, _101);
        inv = sm(inv, 2 + 2, _11);
        inv = sm(inv, 1 + 4, _1111);
        inv = sm(inv, 1 + 4, _1111);
        inv = sm(inv, 4, _1001);
        inv = sm(inv, 2, _11);
        inv = sm(inv, 1 + 4, _1111);
        inv = sm(inv, 1 + 3, _101);
        inv = sm(inv, 3 + 3, _101);
        inv = sm(inv, 3, _111);
        inv = sm(inv, 1 + 4, _1111);
        inv = sm(inv, 2 + 3, _111);
        inv = sm(inv, 2 + 2, _11);
        inv = sm(inv, 1 + 4, _1011);
        inv = sm(inv, 2 + 4, _1011);
        inv = sm(inv, 6 + 4, _1001);
        inv = sm(inv, 2 + 2, _11);
        inv = sm(inv, 3 + 2, _11);
        inv = sm(inv, 3 + 2, _11);
        inv = sm(inv, 1 + 4, _1001);
        inv = sm(inv, 1 + 3, _111);
        inv = sm(inv, 2 + 4, _1111);
        inv = sm(inv, 1 + 4, _1011);
        inv = sm(inv, 3, _101);
        inv = sm(inv, 2 + 4, _1111);
        inv = sm(inv, 3, _101);
        inv = sm(inv, 1 + 2, _11);

        key tmp;
        sc_mul(tmp.bytes, inv.bytes, x.bytes);
        CHECK_THROW_ERR(tmp == identity(), "invert failed");

        return inv;
    }

    static void init_gens()
    {
#if !defined(_WIN32)
        std::lock_guard<std::mutex> lock(init_mutex);
#endif
        static const std::string H_salt("triptych H");

        static bool init_done = false;

        if (init_done) {
            return;
        }

        std::vector<multiexp_data> data;
        data.reserve(max_mn);

        for (size_t i = 0; i < max_mn; i++) {
            std::string hash = H_salt + tools::get_varint_data(i);
            key h;

            hash_data(h, hash.data(), hash.size());
            hash_to_p3(Hi_p3[i], h);
            data.push_back({ZERO, Hi_p3[i]});
        }

        CHECK_THROW_ERR(data.size() == max_mn, "Bad generator vector size!");
        cache = pippenger_init(data,0,0);

        static const std::string U_salt("triptych U");
        key u_h;

        hash_data(u_h, U_salt.data(), U_salt.size());
        hash_to_p3(U_p3, u_h);
        ge_p3_tobytes(U.bytes, &U_p3);

        ge_frombytes_vartime(&G_p3, G.bytes);
        ge_frombytes_vartime(&H_p3, H.bytes);

        init_done = true;
    }

    static void decompose(std::vector<size_t> &r, const size_t i, const size_t n, const size_t m)
    {
        CHECK_THROW_ERR(n > 1, "Bad decomposition parameters!");
        CHECK_THROW_ERR(m > 0, "Bad decomposition parameters!");
        CHECK_THROW_ERR(r.size() >= m, "Bad decomposition result vector size!");

        size_t _i = i;
        size_t _n = n;
        size_t _n1 = 1;

        for (size_t j = 0; j < m; j++) {
            r[j] = (_i % _n) / _n1;
            _i -= _n1*r[j];
            _n1 = _n;
            _n *= n;
        }
    }

    static void com_matrix(std::vector<multiexp_data> &data, const keyM &M, const key &r)
    {
        const size_t m = M.size();
        const size_t n = M[0].size();
        CHECK_THROW_ERR(m*n <= max_mn, "Bad matrix commitment parameters!");
        CHECK_THROW_ERR(data.size() >= m*n + 1, "Bad matrix commitment result vector size!");

        for (size_t j = 0; j < m; j++) {
            for (size_t i = 0; i < n; i++) {
                data[j*n + i] = {M[j][i], Hi_p3[j*n + i]};
            }
        }
        data[m*n] = {r, H_p3};
    }

    static key delta(const size_t x, const size_t y)
    {
        if (x == y) {
            return ONE;
        }

        return ZERO;
    }

    static keyV convolve(const keyV &x, const keyV &y, const size_t m)
    {
        CHECK_THROW_ERR(x.size() >= m, "Bad convolution parameters!");
        CHECK_THROW_ERR(y.size() == 2, "Bad convolution parameters!");

        key temp;
        keyV r;
        r.reserve(m+1);
        r.resize(m+1);

        for (size_t i = 0; i < m+1; i++) {
            r[i] = ZERO;
        }

        for (size_t i = 0; i < m; i++) {
            for (size_t j = 0; j < 2; j++) {
                sc_mul(temp.bytes,x[i].bytes,y[j].bytes);
                sc_add(r[i+j].bytes,r[i+j].bytes,temp.bytes);
            }
        }

        return r;
    }

    triptych triptych_prove(const keyV &M, const keyV &P, const key &C_offset, const size_t l, const key &r, const key &s, const key &message)
    {
        key temp,temp2;

        //CHECK_THROW_ERR(n > 1, "Must have n > 1!");
        //CHECK_THROW_ERR(m > 1, "Must have m > 1!");

        //const size_t N = pow(n,m);
        const size_t N = max_mn;
        const size_t m = 2;
        const size_t n = 6;

        //CHECK_THROW_ERR(m*n <= max_mn, "Size parameters are too large!");
        CHECK_THROW_ERR(M.size() == N, "Public key vector is wrong size!");
        CHECK_THROW_ERR(P.size() == N, "Commitment vector is wrong size!");
        CHECK_THROW_ERR(l < M.size(), "Signing index out of bounds!");
        CHECK_THROW_ERR(scalarmult_base(r) == M[l], "Bad signing key!");

        sub_keys(temp,P[l],C_offset);
        CHECK_THROW_ERR(scalarmult_base(s) == temp, "Bad commitment key!");

        init_gens();

        triptych proof;
        std::vector<multiexp_data> data;
        data.reserve(m*n + 1);
        data.resize(m*n + 1);

        // Begin transcript
        key tr;
        transcript_init(tr);

        // Compute key images
        // J = (1/r)*U
        // K = s*J
        proof.J = scalarmult_key(U,invert(r));
        proof.K = scalarmult_key(proof.J,s);

        // Matrix masks
        key rA = skgen();
        key rB = skgen();
        key rC = skgen();
        key rD = skgen();

        // Commit to zero-sum values
        keyM a = keyM_init(n,m);
        CHECK_THROW_ERR(a.size() == m, "Bad matrix size!");
        CHECK_THROW_ERR(a[0].size() == n, "Bad matrix size!");

        for (size_t j = 0; j < m; j++) {
            a[j][0] = ZERO;

            for (size_t i = 1; i < n; i++) {
                a[j][i] = skgen();
                sc_sub(a[j][0].bytes,a[j][0].bytes,a[j][i].bytes);
            }
        }

        com_matrix(data,a,rA);
        CHECK_THROW_ERR(data.size() == m*n + 1, "Matrix commitment returned unexpected size!");
        proof.A = straus(data);
        CHECK_THROW_ERR(!(proof.A == IDENTITY), "Linear combination unexpectedly returned zero!");

        // Commit to decomposition bits
        std::vector<size_t> decomp_l;
        decomp_l.reserve(m);
        decomp_l.resize(m);
        decompose(decomp_l,l,n,m);

        keyM sigma = keyM_init(n,m);
        CHECK_THROW_ERR(sigma.size() == m, "Bad matrix size!");
        CHECK_THROW_ERR(sigma[0].size() == n, "Bad matrix size!");

        for (size_t j = 0; j < m; j++) {
            for (size_t i = 0; i < n; i++) {
                sigma[j][i] = delta(decomp_l[j],i);
            }
        }

        com_matrix(data,sigma,rB);
        CHECK_THROW_ERR(data.size() == m*n + 1, "Matrix commitment returned unexpected size!");
        proof.B = straus(data);
        CHECK_THROW_ERR(!(proof.B == IDENTITY), "Linear combination unexpectedly returned zero!");

        // Commit to a/sigma relationships
        keyM a_sigma = keyM_init(n,m);
        CHECK_THROW_ERR(a_sigma.size() == m, "Bad matrix size!");
        CHECK_THROW_ERR(a_sigma[0].size() == n, "Bad matrix size!");

        for (size_t j = 0; j < m; j++) {
            for (size_t i = 0; i < n; i++) {
                // a_sigma[j][i] = a[j][i]*(ONE - TWO*sigma[j][i])
                sc_mulsub(a_sigma[j][i].bytes, TWO.bytes, sigma[j][i].bytes, ONE.bytes);
                sc_mul(a_sigma[j][i].bytes, a_sigma[j][i].bytes, a[j][i].bytes);
            }
        }

        com_matrix(data,a_sigma,rC);
        CHECK_THROW_ERR(data.size() == m*n + 1, "Matrix commitment returned unexpected size!");
        proof.C = straus(data);
        CHECK_THROW_ERR(!(proof.C == IDENTITY), "Linear combination unexpectedly returned zero!");

        // Commit to squared a-values
        keyM a_sq = keyM_init(n,m);
        
        for (size_t j = 0; j < m; j++) {
            for (size_t i = 0; i < n; i++) {
                sc_mul(a_sq[j][i].bytes,a[j][i].bytes,a[j][i].bytes);
                sc_mul(a_sq[j][i].bytes,MINUS_ONE.bytes,a_sq[j][i].bytes);
            }
        }

        com_matrix(data,a_sq,rD);
        CHECK_THROW_ERR(data.size() == m*n + 1, "Matrix commitment returned unexpected size!");
        proof.D = straus(data);
        CHECK_THROW_ERR(!(proof.D == IDENTITY), "Linear combination unexpectedly returned zero!");

        // Compute p coefficients
        keyM p = keyM_init(m+1,N);
        CHECK_THROW_ERR(p.size() == N, "Bad matrix size!");
        CHECK_THROW_ERR(p[0].size() == m+1, "Bad matrix size!");
        
        for (size_t k = 0; k < N; k++) {
            std::vector<size_t> decomp_k;
            decomp_k.reserve(m);
            decomp_k.resize(m);
            decompose(decomp_k,k,n,m);

            for (size_t j = 0; j < m+1; j++) {
                p[k][j] = ZERO;
            }

            p[k][0] = a[0][decomp_k[0]];
            p[k][1] = delta(decomp_l[0],decomp_k[0]);

            for (size_t j = 1; j < m; j++) {
                keyV temp;
                temp.reserve(2);
                temp.resize(2);
                temp[0] = a[j][decomp_k[j]];
                temp[1] = delta(decomp_l[j],decomp_k[j]);

                p[k] = convolve(p[k],temp,m);
            }
        }

        // Generate initial proof values
        proof.X = keyV(m);
        proof.Y = keyV(m);

        keyV rho;
        rho.reserve(m);
        rho.resize(m);
        for (size_t j = 0; j < m; j++)
        {
            rho[j] = skgen();
        }

        // Challenge
        proof.K = scalarmult_key(proof.K,INV_EIGHT);
        proof.A = scalarmult_key(proof.A,INV_EIGHT);
        proof.B = scalarmult_key(proof.B,INV_EIGHT);
        proof.C = scalarmult_key(proof.C,INV_EIGHT);
        proof.D = scalarmult_key(proof.D,INV_EIGHT);
        transcript_update_mu(tr,message,M,P,C_offset,proof.J,proof.K,proof.A,proof.B,proof.C,proof.D);
        const key mu = copy(tr);

        key U_scalars;

        for (size_t j = 0; j < m; j++) {
            std::vector<multiexp_data> data_X;
            data_X.reserve(2*N);
            
            U_scalars = ZERO;

            for (size_t k = 0; k < N; k++) {
                // X[j] += p[k][j]*(M[k] + mu*P[k])
                // Y[j] += p[k][j]*U
                data_X.push_back({p[k][j],M[k]});

                sc_mul(temp.bytes,mu.bytes,p[k][j].bytes);
                sub_keys(temp2,P[k],C_offset);
                data_X.push_back({temp,temp2});

                sc_add(U_scalars.bytes,U_scalars.bytes,p[k][j].bytes);
            }

            // X[j] += rho[j]*G
            // Y[j] += rho[j]*J
            add_keys1(proof.X[j], rho[j], straus(data_X));
            CHECK_THROW_ERR(!(proof.X[j] == IDENTITY), "Proof coefficient element should not be zero!");

            proof.Y[j] = scalarmult_key(U,U_scalars);
            key rho_J = scalarmult_key(proof.J,rho[j]);
            add_keys(proof.Y[j],proof.Y[j],rho_J);
            CHECK_THROW_ERR(!(proof.Y[j] == IDENTITY), "Proof coefficient element should not be zero!");
        }

        // Challenge
        for (size_t j = 0; j < m; j++) {
            proof.X[j] = scalarmult_key(proof.X[j],INV_EIGHT);
            proof.Y[j] = scalarmult_key(proof.Y[j],INV_EIGHT);
        }

        CHECK_THROW_ERR(proof.X.size() == m, "Proof coefficient vector is unexpected size!");
        CHECK_THROW_ERR(proof.Y.size() == m, "Proof coefficient vector is unexpected size!");
        transcript_update_x(tr,proof.X,proof.Y);
        const key x = copy(tr);

        // Challenge powers
        keyV x_pow;
        x_pow.reserve(m+1);
        x_pow.resize(m+1);
        x_pow[0] = ONE;

        for (size_t j = 1; j < m+1; j++) {
            sc_mul(x_pow[j].bytes,x_pow[j-1].bytes,x.bytes);
        }

        // Build the f-matrix
        proof.f = keyM_init(n-1,m);
        
        for (size_t j = 0; j < m; j++) {
            for (size_t i = 1; i < n; i++) {
                sc_muladd(proof.f[j][i-1].bytes,sigma[j][i].bytes,x.bytes,a[j][i].bytes);
                CHECK_THROW_ERR(!(proof.f[j][i-1] == ZERO), "Proof matrix element should not be zero!");
            }
        }

        // Build the z-terms
        // zA = rB*x + rA
        // zC = rC*x + rD
        // z = (r + mu*s)*x**m - rho[0]*x**0 - ... - rho[m-1]*x**(m-1)

        sc_muladd(proof.zA.bytes,rB.bytes,x.bytes,rA.bytes);
        CHECK_THROW_ERR(!(proof.zA == ZERO), "Proof scalar element should not be zero!");
        sc_muladd(proof.zC.bytes,rC.bytes,x.bytes,rD.bytes);
        CHECK_THROW_ERR(!(proof.zC == ZERO), "Proof scalar element should not be zero!");

        sc_muladd(proof.z.bytes,mu.bytes,s.bytes,r.bytes);
        sc_mul(proof.z.bytes,proof.z.bytes,x_pow[m].bytes);

        for (size_t j = 0; j < m; j++) {
            sc_mulsub(proof.z.bytes,rho[j].bytes,x_pow[j].bytes,proof.z.bytes);
        }

        CHECK_THROW_ERR(!(proof.z == ZERO), "Proof scalar element should not be zero!");

        // Clear secret prover data
        memwipe(&rA,sizeof(key));
        memwipe(&rB,sizeof(key));
        memwipe(&rC,sizeof(key));
        memwipe(&rD,sizeof(key));

        for (size_t j = 0; j < m; j++) {
            memwipe(a[j].data(),a[j].size()*sizeof(key));
        }

        memwipe(rho.data(),rho.size()*sizeof(key));

        return proof;
    }

    // Verify a batch of Triptych proofs with common input keys
    bool triptych_verify(const keyV &M, const keyV &P, const keyV &C_offsets, std::vector<triptych *> &proofs, const keyV &messages)
    {
        const size_t N = max_mn; // anonymity set size
        const size_t m = 2;
        const size_t n = 6;

        CHECK_THROW_ERR(M.size() == N, "Public key vector is wrong size!");
        CHECK_THROW_ERR(P.size() == N, "Commitment vector is wrong size!");

        const size_t N_proofs = proofs.size(); // number of proofs in batch

        CHECK_THROW_ERR(C_offsets.size() == N_proofs, "Incorrect number of commitment offsets!");
        CHECK_THROW_ERR(messages.size() == N_proofs, "Incorrect number of messages!");

        // Per-proof checks
        for (triptych *p: proofs)
        {
            triptych &proof = *p;

            CHECK_THROW_ERR(!(proof.J == IDENTITY), "Proof group element should not be zero!");
            CHECK_THROW_ERR(proof.X.size() == m, "Bad proof vector size!");
            CHECK_THROW_ERR(proof.Y.size() == m, "Bad proof vector size!");
            CHECK_THROW_ERR(proof.f.size() == m, "Bad proof matrix size!");
            
            for (size_t j = 0; j < m; j++) {
                CHECK_THROW_ERR(proof.f[j].size() == n-1, "Bad proof matrix size!");

                for (size_t i = 0; i < n-1; i++) {
                    CHECK_THROW_ERR(sc_check(proof.f[j][i].bytes) == 0, "Bad scalar element in proof!");
                }
            }

            CHECK_THROW_ERR(sc_check(proof.zA.bytes) == 0, "Bad scalar element in proof!");
            CHECK_THROW_ERR(!(proof.zA == ZERO), "Proof scalar element should not be zero!");
            CHECK_THROW_ERR(sc_check(proof.zC.bytes) == 0, "Bad scalar element in proof!");
            CHECK_THROW_ERR(!(proof.zC == ZERO), "Proof scalar element should not be zero!");
            CHECK_THROW_ERR(sc_check(proof.z.bytes) == 0, "Bad scalar element in proof!");
            CHECK_THROW_ERR(!(proof.z == ZERO), "Proof scalar element should not be zero!");
        }

        init_gens();
        key temp;

        // Holds final check data (Q proofs)
        //
        // Index data:
        // 0            m*n-1       Hi[i]
        // m*n                      H
        // m*n+1        m*n+N       M[i]
        // m*n+N+1      m*n+2*N     P[i]
        // m*n+2*N+1                U
        // m*n+2*N+2                G
        // ... then per-proof data
        std::vector<multiexp_data> data;
        data.reserve((m*n + 1) + (2*N + 2) + N_proofs*(2*m + 7));
        data.resize((m*n + 1) + (2*N + 2)); // set up for all common elements

        // Data for {Hi},H
        for (size_t i = 0; i < m*n; i++) {
            data[i] = {ZERO,Hi_p3[i]};
        }

        data[m*n] = {ZERO,H_p3};

        // Data for {M},{P}
        for (size_t k = 0; k < N; k++) {
            data[m*n+1+k] = {ZERO,M[k]};
            data[m*n+N+1+k] = {ZERO,P[k]};
        }

        // Data for U
        data[m*n+2*N+1] = {ZERO,U_p3};

        // Data for G
        data[m*n+2*N+2] = {ZERO,G_p3};

        // Start per-proof data assembly
        for (size_t i_proofs = 0; i_proofs < N_proofs; i_proofs++) {
            triptych &proof = *proofs[i_proofs];

            // Per-proof random weights
            key w1 = ZERO;
            key w2 = ZERO;
            key w3 = ZERO;
            key w4 = ZERO;

            while (w1 == ZERO || w2 == ZERO || w3 == ZERO || w4 == ZERO) {
                w1 = skgen();
                w2 = skgen();
                w3 = skgen();
                w4 = skgen();
            }

            // Transcript
            key tr;
            transcript_init(tr);
            transcript_update_mu(tr,messages[i_proofs],M,P,C_offsets[i_proofs],proof.J,proof.K,proof.A,proof.B,proof.C,proof.D);
            const key mu = copy(tr);
            transcript_update_x(tr,proof.X,proof.Y);
            const key x = copy(tr);

            // Recover proof elements
            ge_p3 K_p3;
            ge_p3 A_p3;
            ge_p3 B_p3;
            ge_p3 C_p3;
            ge_p3 D_p3;
            std::vector<ge_p3> X_p3;
            std::vector<ge_p3> Y_p3;

            X_p3.reserve(m);
            X_p3.resize(m);
            Y_p3.reserve(m);
            Y_p3.resize(m);
            scalarmult_8(K_p3,proof.K);
            scalarmult_8(A_p3,proof.A);
            scalarmult_8(B_p3,proof.B);
            scalarmult_8(C_p3,proof.C);
            scalarmult_8(D_p3,proof.D);

            for (size_t j = 0; j < m; j++) {
                scalarmult_8(X_p3[j],proof.X[j]);
                scalarmult_8(Y_p3[j],proof.Y[j]);
            }

            // Challenge powers (negated)
            keyV minus_x;
            minus_x.reserve(m);
            minus_x.resize(m);
            minus_x[0] = MINUS_ONE;
            for (size_t j = 1; j < m; j++) {
                sc_mul(minus_x[j].bytes,minus_x[j-1].bytes,x.bytes);
            }

            // Reconstruct the f-matrix
            keyM f = keyM_init(n,m);
            for (size_t j = 0; j < m; j++) {
                f[j][0] = x;

                for (size_t i = 1; i < n; i++) {
                    CHECK_THROW_ERR(!(proof.f[j][i-1] == ZERO), "Proof matrix element should not be zero!");
                    f[j][i] = proof.f[j][i-1];
                    sc_sub(f[j][0].bytes,f[j][0].bytes,f[j][i].bytes);
                }

                CHECK_THROW_ERR(!(f[j][0] == ZERO), "Proof matrix element should not be zero!");
            }

            // Matrix generators
            for (size_t j = 0; j < m; j++) {
                for (size_t i = 0; i < n; i++) {
                    // Hi: w1*f + w2*f*(x-f) = w1*f + w2*f*x - w2*f*f
                    key Hi_scalar;
                    sc_mul(Hi_scalar.bytes,w1.bytes,f[j][i].bytes);

                    sc_mul(temp.bytes,w2.bytes,f[j][i].bytes);
                    sc_mul(temp.bytes,temp.bytes,x.bytes);
                    sc_add(Hi_scalar.bytes,Hi_scalar.bytes,temp.bytes);

                    sc_mul(temp.bytes,MINUS_ONE.bytes,w2.bytes);
                    sc_mul(temp.bytes,temp.bytes,f[j][i].bytes);
                    sc_mul(temp.bytes,temp.bytes,f[j][i].bytes);
                    sc_add(Hi_scalar.bytes,Hi_scalar.bytes,temp.bytes);

                    sc_add(data[j*n + i].scalar.bytes,data[j*n + i].scalar.bytes,Hi_scalar.bytes);
                }
            }

            // H: w1*zA + w2*zC
            sc_muladd(data[m*n].scalar.bytes,w1.bytes,proof.zA.bytes,data[m*n].scalar.bytes);
            sc_muladd(data[m*n].scalar.bytes,w2.bytes,proof.zC.bytes,data[m*n].scalar.bytes);

            // A,B,C,D
            // A: -w1
            // B: -w1*x
            // C: -w2*x
            // D: -w2
            sc_mul(temp.bytes,MINUS_ONE.bytes,w1.bytes);
            data.push_back({temp,A_p3});

            sc_mul(temp.bytes,temp.bytes,x.bytes);
            data.push_back({temp,B_p3});

            sc_mul(temp.bytes,MINUS_ONE.bytes,w2.bytes);
            data.push_back({temp,D_p3});

            sc_mul(temp.bytes,temp.bytes,x.bytes);
            data.push_back({temp,C_p3});

            // M,P
            // M[k]: w3*t
            // P[k]: w3*t*mu
            key sum_t = ZERO;
            for (size_t k = 0; k < N; k++) {
                key t = ONE;
                std::vector<size_t> decomp_k;
                decomp_k.reserve(m);
                decomp_k.resize(m);
                decompose(decomp_k,k,n,m);

                for (size_t j = 0; j < m; j++) {
                    sc_mul(t.bytes,t.bytes,f[j][decomp_k[j]].bytes);
                }

                sc_mul(temp.bytes,w3.bytes,t.bytes);
                sc_add(data[m*n+1+k].scalar.bytes,data[m*n+1+k].scalar.bytes,temp.bytes);

                sc_mul(temp.bytes,temp.bytes,mu.bytes);
                sc_add(data[m*n+N+1+k].scalar.bytes,data[m*n+N+1+k].scalar.bytes,temp.bytes);

                sc_add(sum_t.bytes,sum_t.bytes,t.bytes);
            }

            // C_offsets[i_proofs]: -w3*mu*sum_t
            sc_mul(temp.bytes,MINUS_ONE.bytes,w3.bytes);
            sc_mul(temp.bytes,temp.bytes,mu.bytes);
            sc_mul(temp.bytes,temp.bytes,sum_t.bytes);
            data.push_back({temp,C_offsets[i_proofs]});

            // U: w4*sum_t
            sc_mul(temp.bytes,w4.bytes,sum_t.bytes);
            sc_add(data[m*n+2*N+1].scalar.bytes,data[m*n+2*N+1].scalar.bytes,temp.bytes);

            // K: w4*sum_t*mu
            sc_mul(temp.bytes,temp.bytes,mu.bytes);
            data.push_back({temp,K_p3});

            for (size_t j = 0; j < m; j++) {
                // X[j]: -w3*x**j
                sc_mul(temp.bytes,w3.bytes,minus_x[j].bytes);
                data.push_back({temp,X_p3[j]});

                // Y[j]: -w4*x**j
                sc_mul(temp.bytes,w4.bytes,minus_x[j].bytes);
                data.push_back({temp,Y_p3[j]});
            }

            // G: -w3*z
            sc_mul(temp.bytes,MINUS_ONE.bytes,proof.z.bytes);
            sc_mul(temp.bytes,temp.bytes,w3.bytes);
            sc_add(data[m*n+2*N+2].scalar.bytes,data[m*n+2*N+2].scalar.bytes,temp.bytes);

            // J: -w4*z
            sc_mul(temp.bytes,MINUS_ONE.bytes,proof.z.bytes);
            sc_mul(temp.bytes,temp.bytes,w4.bytes);
            data.push_back({temp,proof.J});
        }

        // Final check
        CHECK_THROW_ERR(data.size() == (m*n + 1) + (2*N + 2) + N_proofs*(2*m + 7), "Final proof data is incorrect size!");
        if (!(pippenger(data,cache,m*n,get_pippenger_c(data.size())) == IDENTITY)) {
            return false;
        }

        return true;
    }
}