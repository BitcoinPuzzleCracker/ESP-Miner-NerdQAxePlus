// ============================================================================
//  extranonce2_test.cpp
//  Standalone test for random extranonce2 generation scheme.
//
//  Scheme:
//    - start at MIN_HEX_DIGITS significant hex digits (default 4)
//    - generate a random value limited to N significant hex digits
//    - emit TRAILING  (right-aligned, zero-pad left)
//    - emit LEADING   (left-aligned,  zero-pad right, SAME random value)
//    - N++ ; repeat until N > max_hex (= extranonce_2_len * 2)
//    - wrap back to MIN_HEX_DIGITS
//    - on pool switch (extranonce_2_len change) : state RESETS
//
//  Compile:   g++ -O2 -Wall -o test extranonce2_test.cpp
//  Run:       ./test
// ============================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// -------- CONFIG -------------------------------------------------------------
static constexpr int  MIN_HEX_DIGITS                 = 4;   // start met 4 significante hex-cijfers
static constexpr int  RANDOMS_PER_SIZE               = 1;   // random waarden per cijfer-grootte
static constexpr bool SKIP_DUPLICATE_AT_FULL_WIDTH   = false; // true = geen 2e emit bij volle breedte

// -------- DETERMINISTIC RNG (vervang door esp_random() op ESP32) ------------
static uint64_t g_rng_state = 0x123456789ABCDEFULL;
static uint32_t fake_esp_random() {
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 7;
    g_rng_state ^= g_rng_state << 17;
    return (uint32_t)(g_rng_state >> 16);
}
static uint64_t fake_esp_random64() {
    return ((uint64_t)fake_esp_random() << 32) | fake_esp_random();
}

// -------- STATE MACHINE ------------------------------------------------------
struct EnonceRandomState {
    int      max_hex      = 0;                 // = extranonce_2_len * 2
    int      hex_digits   = MIN_HEX_DIGITS;    // huidige significante hex-lengte
    int      randoms_done = 0;                 // hoeveel random waarden al bij deze size
    bool     leading      = false;             // false = trailing, true = leading
    uint64_t last_random  = 0;                 // zelfde random voor beide oriëntaties
};

static void enonce_state_init(EnonceRandomState &s, int extranonce_2_len) {
    s.max_hex = extranonce_2_len * 2;
    int start = (s.max_hex >= MIN_HEX_DIGITS) ? MIN_HEX_DIGITS : s.max_hex;
    if (start < 1) start = 1;
    s.hex_digits   = start;
    s.randoms_done = 0;
    s.leading      = false;
    s.last_random  = 0;
}

static uint64_t next_random_extranonce2(EnonceRandomState &s) {
    if (s.max_hex <= 0) return 0;

    // veiligheids-clamps (pool met te kleine / ontbrekende enonce)
    if (s.hex_digits < 1) s.hex_digits = 1;
    if (s.hex_digits > s.max_hex) {
        int start = (s.max_hex >= MIN_HEX_DIGITS) ? MIN_HEX_DIGITS : s.max_hex;
        if (start < 1) start = 1;
        s.hex_digits   = start;
        s.randoms_done = 0;
        s.leading      = false;
    }

    int      digits = s.hex_digits;
    uint64_t value;

    if (!s.leading) {
        // (1) nieuwe random, beperkt tot 'digits' hex-cijfers
        uint64_t mask = (digits >= 16) ? ~0ULL : ((1ULL << (digits * 4)) - 1);
        s.last_random = fake_esp_random64() & mask;
        value = s.last_random;             // trailing: right-aligned
        s.leading = true;

        if (SKIP_DUPLICATE_AT_FULL_WIDTH && digits == s.max_hex) {
            s.leading = false;
            s.randoms_done++;
            if (s.randoms_done >= RANDOMS_PER_SIZE) { s.hex_digits++; s.randoms_done = 0; }
        }
    } else {
        // (2) zelfde random, leading-shifted
        int shift = (s.max_hex - digits) * 4;
        value = (shift >= 64) ? 0 : (s.last_random << shift);
        s.leading = false;
        s.randoms_done++;
        if (s.randoms_done >= RANDOMS_PER_SIZE) {
            s.hex_digits++;
            s.randoms_done = 0;
        }
    }
    return value;
}

// -------- FORMAT -------------------------------------------------------------
static void format_hex(char *out, size_t n, uint64_t v, int width) {
    if (width >= 16) snprintf(out, n, "%016llx", (unsigned long long)v);
    else             snprintf(out, n, "%0*llx",  width, (unsigned long long)v);
}

// -------- TEST RUNNER --------------------------------------------------------
static void run_test(int extranonce_2_len, int num_jobs) {
    printf("\n+--------------------------------------------------------------------+\n");
    printf("| POOL: extranonce_2_len = %d byte(s)    max_hex = %2d                |\n",
           extranonce_2_len, extranonce_2_len * 2);
    printf("+--------------------------------------------------------------------+\n");
    printf(" idx | digits | orient   | %-16s | note\n", "extranonce2");
    printf("-----+--------+----------+------------------+---------------------------------\n");

    EnonceRandomState s;
    enonce_state_init(s, extranonce_2_len);

    for (int i = 0; i < num_jobs; i++) {
        int      d            = s.hex_digits;
        bool     was_leading  = s.leading;
        uint64_t v            = next_random_extranonce2(s);
        char     buf[32];
        format_hex(buf, sizeof(buf), v, s.max_hex);

        const char *note;
        if (!was_leading) note = (d == s.max_hex) ? "new random | full width"
                                                  : "new random | trailing pad";
        else              note = (d == s.max_hex) ? "same random | full width (identical)"
                                                  : "same random | leading pad";

        printf(" %3d | %6d | %-8s | %-16s | %s\n",
               i, d, was_leading ? "leading" : "trailing", buf, note);
    }
}

// -------- WORKED EXAMPLE (exact jouw voorbeeld) ------------------------------
static void show_worked_example() {
    printf("\n+--------------------------------------------------------------------+\n");
    printf("| WORKED EXAMPLE (vaste waarden, precies jouw voorbeeld)             |\n");
    printf("| Veld = 16 hex chars (= 8-byte extranonce2)                        |\n");
    printf("+--------------------------------------------------------------------+\n");

    const int max_hex = 16;
    struct { int digits; uint64_t val; } cases[] = {
        {4,  0x3951ULL},
        {5,  0x04A2BULL},
        {6,  0x12CDEFULL},
        {7,  0x0AABBCCULL},
        {8,  0xDEADBEEFULL},
        {16, 0x123456789ABCDEF0ULL},
    };
    for (auto &c : cases) {
        int      shift    = (max_hex - c.digits) * 4;
        uint64_t trailing = c.val;
        uint64_t leading  = (shift >= 64) ? 0 : (c.val << shift);
        char     tbuf[32], lbuf[32];
        format_hex(tbuf, sizeof(tbuf), trailing, max_hex);
        format_hex(lbuf, sizeof(lbuf), leading,  max_hex);
        printf("\n  digits = %2d , random = 0x%llx\n",
               c.digits, (unsigned long long)c.val);
        printf("    TRAILING -> %s\n", tbuf);
        printf("    LEADING  -> %s%s\n", lbuf,
               (trailing == leading) ? "   (identiek: volle breedte)" : "");
    }
}

// -------- POOL SWITCH TEST ---------------------------------------------------
static EnonceRandomState g_pools[4];
static int              g_pool_len[4] = {-1, -1, -1, -1};

static uint64_t pool_next(int pool, int pool_len) {
    if (g_pool_len[pool] != pool_len) {
        printf("  [pool %d: extranonce_2_len %d -> %d, state RESET]\n",
               pool, g_pool_len[pool], pool_len);
        enonce_state_init(g_pools[pool], pool_len);
        g_pool_len[pool] = pool_len;
    }
    return next_random_extranonce2(g_pools[pool]);
}

static void run_pool_switch_test() {
    printf("\n+--------------------------------------------------------------------+\n");
    printf("| POOL SWITCHING TEST (failover simulatie)                          |\n");
    printf("| pools: 0 = 8-byte, 1 = 4-byte, 2 = 2-byte                        |\n");
    printf("+--------------------------------------------------------------------+\n\n");

    struct { int pool; int len; } seq[] = {
        {0,8},{0,8},{0,8},
        {1,4},{1,4},{1,4},
        {2,2},{2,2},{2,2},
        {0,8},{0,8},   // terug naar pool 0 -> gaat verder waar hij was
        {1,4},{1,4},
    };
    for (auto &e : seq) {
        uint64_t v = pool_next(e.pool, e.len);
        char     buf[32];
        format_hex(buf, sizeof(buf), v, e.len * 2);
        printf("pool %d (len=%d) -> %s\n", e.pool, e.len, buf);
    }
}

int main() {
    printf("======================================================================\n");
    printf(" EXTRANONCE2 RANDOM GENERATOR -- TEST HARNESS\n");
    printf("======================================================================\n");
    printf(" MIN_HEX_DIGITS   = %d\n", MIN_HEX_DIGITS);
    printf(" RANDOMS_PER_SIZE = %d\n", RANDOMS_PER_SIZE);
    printf(" SKIP_DUPLICATE_AT_FULL_WIDTH = %s\n",
           SKIP_DUPLICATE_AT_FULL_WIDTH ? "true" : "false");

    show_worked_example();

    run_test(2, 12);   // 2-byte pool -> max_hex = 4
    run_test(3, 20);   // 3-byte pool -> max_hex = 6
    run_test(4, 20);   // 4-byte pool -> max_hex = 8   (uint32_t grens op ESP32)
    run_test(8, 30);   // 8-byte pool -> max_hex = 16  (vereist uint64_t refactor)

    run_pool_switch_test();

    printf("\n======================================================================\n");
    return 0;
}
