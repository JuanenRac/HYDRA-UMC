/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for flash_layer.h (one flash layer for the three bootloaders) and adoption_guard.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The layer runs on a simulated flash with the two geometries of the boards (the G474: erase unit 2 KB, program unit 8 bytes; the H745: erase unit
 * 128 KB, program unit 32 bytes), with the rules of the real parts that matter here: an erase sets bytes to 0xFF, a program may only be done on a
 * unit that is blank, and a power cut in the middle of a unit leaves it half written. Compiled once per boot directory (the three copies of the file
 * must behave the same); the part about adoption_guard.h exists only where that file does (HYDRA_TEST_ADOPTION, the G474). Evidence: simulator. The
 * real cuts of the power and the double-word ECC are bench rows 24 and 25 of docs/BENCH_TESTS.md.
 */
#include <stdio.h>
#include <string.h>

#include "flash_layer.h"
#ifdef HYDRA_TEST_ADOPTION
#include "adoption_guard.h"
#endif

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

#define SIM_SIZE (512u * 1024u)

typedef struct {
    uint8_t mem[SIM_SIZE];
    FlashLayerGeometry_t geometry;
    int programs;            /* program calls so far */
    int cut_at_program;      /* the call (counting from 1) that loses power half way; 0 = never */
    int fail_program_at;     /* the call that returns an error without writing; 0 = never */
    int flip_read_after;     /* after this program call, reads return a flipped bit in the unit just written; 0 = never */
    uint32_t flipped_address;
    int dead;                /* power is gone: every call fails */
} SimFlash;

static int sim_erase(void *context, uint32_t address, uint32_t length)
{
    SimFlash *f = (SimFlash *)context;
    if (f->dead) return 1;
    memset(&f->mem[address - f->geometry.base], 0xFF, length);
    return 0;
}

static int sim_program(void *context, uint32_t address, const uint8_t *data, uint32_t length)
{
    SimFlash *f = (SimFlash *)context;
    if (f->dead) return 1;
    f->programs++;
    if (f->fail_program_at == f->programs) return 1;
    uint8_t *place = &f->mem[address - f->geometry.base];
    for (uint32_t i = 0; i < length; i++) {
        if (place[i] != 0xFFu) return 1;   /* programming a unit that is not blank: the real part reports an error */
    }
    if (f->cut_at_program == f->programs) {
        memcpy(place, data, length / 2u);  /* half of the unit is written, then the power goes */
        f->dead = 1;
        return 1;
    }
    memcpy(place, data, length);
    if (f->flip_read_after == f->programs) f->flipped_address = address;
    return 0;
}

static int sim_read(void *context, uint32_t address, uint8_t *out, uint32_t length)
{
    SimFlash *f = (SimFlash *)context;
    if (f->dead) return 1;
    memcpy(out, &f->mem[address - f->geometry.base], length);
    if (f->flipped_address != 0u && address == f->flipped_address) out[0] ^= 0x01u;
    return 0;
}

static void sim_init(SimFlash *f, const FlashLayerGeometry_t *g)
{
    memset(f, 0, sizeof *f);
    f->geometry = *g;
    f->geometry.size = SIM_SIZE;
    memset(f->mem, 0xFF, SIM_SIZE);
}

static FlashLayerOps_t ops_for(SimFlash *f)
{
    FlashLayerOps_t ops = {&f->geometry, f, sim_erase, sim_program, sim_read};
    return ops;
}

static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 31u + 7u); }

static void run_geometry(const char *name, const FlashLayerGeometry_t *g)
{
    static SimFlash flash;
    uint8_t data[1000];
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = pattern(i);
    const uint32_t base = g->base;
    const uint32_t eu = g->erase_unit;
    const uint32_t pu = g->program_unit;
    char label[96];
#define CHECK(cond, text) do { snprintf(label, sizeof label, "%s: %s", name, text); TEST_ASSERT(cond, label); } while (0)

    /* ranges */
    CHECK(FlashLayer_InRange(g, base, 1u), "the first byte is in range");
    CHECK(FlashLayer_InRange(g, base + g->size - 1u, 1u), "the last byte is in range");
    CHECK(!FlashLayer_InRange(g, base + g->size, 1u), "one past the end is out");
    CHECK(!FlashLayer_InRange(g, base - 1u, 1u), "one before the start is out");
    CHECK(!FlashLayer_InRange(g, base, 0u), "a zero length is out");
    CHECK(!FlashLayer_InRange(g, base + g->size - 4u, 8u), "a range that crosses the end is out");
    CHECK(!FlashLayer_InRange(g, base + 8u, 0xFFFFFFFFu), "a range that would wrap is out");
    CHECK(FlashLayer_Check(g, base + pu + 1u, pu, pu) == FLASH_LAYER_UNALIGNED, "an unaligned address is refused");
    CHECK(FlashLayer_Check(g, base, pu + 1u, pu) == FLASH_LAYER_UNALIGNED, "an unaligned length is refused");

    /* erase and write, with the padding of the last unit */
    sim_init(&flash, g);
    FlashLayerOps_t ops = ops_for(&flash);
    CHECK(FlashLayer_Erase(&ops, base + 1u, eu) == FLASH_LAYER_UNALIGNED, "an erase not on a unit is refused");
    CHECK(FlashLayer_Erase(&ops, base + SIM_SIZE, eu) == FLASH_LAYER_BAD_RANGE, "an erase outside the flash is refused");
    CHECK(FlashLayer_Write(&ops, base + 3u, data, 16u) == FLASH_LAYER_UNALIGNED, "a write not on a unit is refused");
    CHECK(FlashLayer_Write(&ops, base, data, 0u) == FLASH_LAYER_BAD_RANGE, "a write of nothing is refused");
    CHECK(FlashLayer_Write(&ops, base, data, 1000u) == FLASH_LAYER_OK, "a write of 1000 bytes into erased flash works");
    CHECK(memcmp(&flash.mem[0], data, 1000u) == 0, "the bytes are there");
    {
        const uint32_t padded = ((1000u + pu - 1u) / pu) * pu;
        int pad_ok = 1;
        for (uint32_t i = 1000u; i < padded; i++) pad_ok &= flash.mem[i] == 0xFFu;
        CHECK(pad_ok, "the last unit is padded with 0xFF");
    }
    CHECK(FlashLayer_Write(&ops, base, data, 16u) == FLASH_LAYER_NOT_BLANK, "a second write over the same place is refused before any program call");
    {
        uint8_t blank = 9u;
        CHECK(FlashLayer_IsBlank(&ops, base, pu, &blank) == FLASH_LAYER_OK && blank == 0u, "a written unit is not blank");
        CHECK(FlashLayer_IsBlank(&ops, base + eu, pu, &blank) == FLASH_LAYER_OK && blank == 1u, "an untouched unit is blank");
    }
    CHECK(FlashLayer_Erase(&ops, base, eu) == FLASH_LAYER_OK, "the erase works");
    CHECK(FlashLayer_Write(&ops, base, data, 100u) == FLASH_LAYER_OK, "and the place can be written again");

    /* a program call that fails, and a read-back that differs */
    sim_init(&flash, g);
    ops = ops_for(&flash);
    flash.fail_program_at = 2;
    CHECK(FlashLayer_Write(&ops, base, data, 64u) == FLASH_LAYER_PROGRAM_FAILED, "a failing program call is reported");
    sim_init(&flash, g);
    ops = ops_for(&flash);
    flash.flip_read_after = 1;
    flash.flipped_address = 0u;
    {
        /* the flip is armed by the program call itself: the first unit is read back with a bit changed */
        const FlashLayerStatus st = FlashLayer_Write(&ops, base, data, 64u);
        CHECK(st == FLASH_LAYER_VERIFY_FAILED, "a read-back that differs is a failure, not a silent bad image");
    }

    /* a copy, then a copy that loses the power at every program unit: the source is never touched and a cut is never reported as a success */
    {
        const uint32_t source = base + eu;
        const uint32_t destination = base + 2u * eu;
        const uint32_t length = pu * 40u;
        uint8_t image[2048];
        for (uint32_t i = 0; i < sizeof image; i++) image[i] = pattern(i + 5u);
        sim_init(&flash, g);
        ops = ops_for(&flash);
        CHECK(FlashLayer_Write(&ops, source, image, length) == FLASH_LAYER_OK, "the source is written");
        CHECK(FlashLayer_Copy(&ops, destination, source, length) == FLASH_LAYER_OK, "a copy works");
        CHECK(memcmp(&flash.mem[destination - base], &flash.mem[source - base], length) == 0, "the destination equals the source");
        const int programs_for_copy = flash.programs - (int)((length + pu - 1u) / pu);
        const int total_units = (int)((length + pu - 1u) / pu);
        CHECK(programs_for_copy == total_units, "a copy programs as many units as the image has");
        for (int cut = 1; cut <= total_units; cut++) {
            sim_init(&flash, g);
            ops = ops_for(&flash);
            (void)FlashLayer_Write(&ops, source, image, length);
            flash.programs = 0;
            flash.cut_at_program = cut;
            const FlashLayerStatus st = FlashLayer_Copy(&ops, destination, source, length);
            if (st == FLASH_LAYER_OK) { CHECK(0, "a cut copy must not report success"); break; }
            flash.dead = 0;
            int source_intact = memcmp(&flash.mem[source - base], image, length) == 0;
            if (!source_intact) { CHECK(0, "a cut copy must leave the source intact"); break; }
            /* the torn unit is not blank, so the layer refuses to write over it instead of pretending it is erased */
            uint8_t blank = 1u;
            const uint32_t torn = destination + (uint32_t)(cut - 1) * pu;
            if (FlashLayer_IsBlank(&ops, torn, pu, &blank) != FLASH_LAYER_OK || blank != 0u) { CHECK(0, "the torn unit is seen as not blank"); break; }
            if (FlashLayer_Write(&ops, torn, image, pu) != FLASH_LAYER_NOT_BLANK) { CHECK(0, "and is not written over"); break; }
        }
    }
    /* a copy with an unaligned or crossing range */
    sim_init(&flash, g);
    ops = ops_for(&flash);
    CHECK(FlashLayer_Copy(&ops, base + eu, base + 3u, 16u) == FLASH_LAYER_UNALIGNED, "a copy from an unaligned source is refused");
    CHECK(FlashLayer_Copy(&ops, base + eu + pu, base, 16u) == FLASH_LAYER_UNALIGNED, "a copy to a destination that is not on an erase unit is refused");
#undef CHECK
}

#ifdef HYDRA_TEST_ADOPTION
static void test_adoption_guard(void)
{
    TEST_ASSERT(Adopt_RdpLevel(0xAAu) == 0u, "0xAA is level 0");
    TEST_ASSERT(Adopt_RdpLevel(0xBBu) == 1u, "0xBB is level 1");
    TEST_ASSERT(Adopt_RdpLevel(0x00u) == 1u, "0x00 is level 1, not level 0");
    TEST_ASSERT(Adopt_RdpLevel(0xFFu) == 1u, "0xFF is level 1");
    TEST_ASSERT(Adopt_RdpLevel(0xCCu) == 2u, "0xCC is level 2");
    TEST_ASSERT(Adopt_RdpLevel(0x12345600u | 0xAAu) == 0u, "only the low byte of the register counts");
    TEST_ASSERT(Adopt_ReadbackAllowed(0xBBu, 0u) == 1u, "with the gate off the read-back is served on a protected board (the earlier behaviour)");
    TEST_ASSERT(Adopt_ReadbackAllowed(0xAAu, 1u) == 1u, "with the gate on, level 0 is served");
    TEST_ASSERT(Adopt_ReadbackAllowed(0xBBu, 1u) == 0u, "with the gate on, level 1 is refused");
    TEST_ASSERT(Adopt_ReadbackAllowed(0xCCu, 1u) == 0u, "with the gate on, level 2 is refused");
    TEST_ASSERT(Adopt_ReadbackAllowed(0x00u, 1u) == 0u, "with the gate on, any value but 0xAA is refused");
    TEST_ASSERT(HYDRA_READBACK_GATE == 0, "the switch ships off");

    AdoptRecord_t wanted;
    memset(&wanted, 0, sizeof wanted);
    wanted.size = 0x1000u;
    wanted.hardware_id = 0x474u;
    wanted.crc32 = 0xDEADBEEFu;
    for (int i = 0; i < 32; i++) wanted.sha256[i] = (uint8_t)(i * 7 + 1);
    AdoptRecord_t read_back = wanted;
    TEST_ASSERT(Adopt_RecordMatches(&wanted, &read_back) == 1u, "an identical record matches");
    read_back.size ^= 1u;
    TEST_ASSERT(Adopt_RecordMatches(&wanted, &read_back) == 0u, "a different size does not");
    read_back = wanted; read_back.hardware_id ^= 0x100u;
    TEST_ASSERT(Adopt_RecordMatches(&wanted, &read_back) == 0u, "a different hardware identifier does not");
    read_back = wanted; read_back.crc32 ^= 0x80000000u;
    TEST_ASSERT(Adopt_RecordMatches(&wanted, &read_back) == 0u, "a different CRC does not");
    for (int i = 0; i < 32; i++) {
        for (int bit = 0; bit < 8; bit++) {
            read_back = wanted;
            read_back.sha256[i] ^= (uint8_t)(1u << bit);
            if (Adopt_RecordMatches(&wanted, &read_back) != 0u) { TEST_ASSERT(0, "one flipped bit of the digest must not match"); i = 32; break; }
        }
    }
}
#endif

int main(void)
{
    printf("HYDRA-UMC host logic tests - flash_layer.h and adoption_guard.h\n");
    TEST_ASSERT(FlashLayer_GeometryG474()->erase_unit == 2048u && FlashLayer_GeometryG474()->program_unit == 8u && FlashLayer_GeometryG474()->size == 0x80000u, "G474 geometry");
    TEST_ASSERT(FlashLayer_GeometryH745()->erase_unit == 0x20000u && FlashLayer_GeometryH745()->program_unit == 32u && FlashLayer_GeometryH745()->size == 0x200000u, "H745 geometry");
    TEST_ASSERT(FlashLayer_GeometryG474()->program_unit <= FLASH_LAYER_MAX_PROGRAM_UNIT && FlashLayer_GeometryH745()->program_unit <= FLASH_LAYER_MAX_PROGRAM_UNIT,
                "the buffers hold the largest program unit");
    run_geometry("G474", FlashLayer_GeometryG474());
    run_geometry("H745", FlashLayer_GeometryH745());
#ifdef HYDRA_TEST_ADOPTION
    test_adoption_guard();
#endif
    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
