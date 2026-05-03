

#ifndef LIB_CRC32_MULTIPLIERS_H
#define LIB_CRC32_MULTIPLIERS_H

#include "common_defs.h"

#define CRC32_X159_MODG 0xae689191
#define CRC32_X95_MODG 0xccaa009e

#define CRC32_X287_MODG 0xf1da05aa
#define CRC32_X223_MODG 0x81256527

#define CRC32_X415_MODG 0x3db1ecdc
#define CRC32_X351_MODG 0xaf449247

#define CRC32_X543_MODG 0x8f352d95
#define CRC32_X479_MODG 0x1d9513d7

#define CRC32_X671_MODG 0x1c279815
#define CRC32_X607_MODG 0xae0b5394

#define CRC32_X799_MODG 0xdf068dc2
#define CRC32_X735_MODG 0x57c54819

#define CRC32_X927_MODG 0x31f8303f
#define CRC32_X863_MODG 0x0cbec0ed

#define CRC32_X1055_MODG 0x33fff533
#define CRC32_X991_MODG 0x910eeec1

#define CRC32_X1183_MODG 0x26b70c3d
#define CRC32_X1119_MODG 0x3f41287a

#define CRC32_X1311_MODG 0xe3543be0
#define CRC32_X1247_MODG 0x9026d5b1

#define CRC32_X1439_MODG 0x5a1bb05d
#define CRC32_X1375_MODG 0xd1df2327

#define CRC32_X1567_MODG 0x596c8d81
#define CRC32_X1503_MODG 0xf5e48c85

#define CRC32_X1695_MODG 0x682bdd4f
#define CRC32_X1631_MODG 0x3c656ced

#define CRC32_X1823_MODG 0x4a28bd43
#define CRC32_X1759_MODG 0xfe807bbd

#define CRC32_X1951_MODG 0x0077f00d
#define CRC32_X1887_MODG 0x1f0c2cdd

#define CRC32_X2079_MODG 0xce3371cb
#define CRC32_X2015_MODG 0xe95c1271

#define CRC32_X2207_MODG 0xa749e894
#define CRC32_X2143_MODG 0xb918a347

#define CRC32_X2335_MODG 0x2c538639
#define CRC32_X2271_MODG 0x71d54a59

#define CRC32_X2463_MODG 0x32b0733c
#define CRC32_X2399_MODG 0xff6f2fc2

#define CRC32_X2591_MODG 0x0e9bd5cc
#define CRC32_X2527_MODG 0xcec97417

#define CRC32_X2719_MODG 0x76278617
#define CRC32_X2655_MODG 0x1c63267b

#define CRC32_X2847_MODG 0xc51b93e3
#define CRC32_X2783_MODG 0xf183c71b

#define CRC32_X2975_MODG 0x7eaed122
#define CRC32_X2911_MODG 0x9b9bdbd0

#define CRC32_X3103_MODG 0x2ce423f1
#define CRC32_X3039_MODG 0xd31343ea

#define CRC32_X3231_MODG 0x8b8d8645
#define CRC32_X3167_MODG 0x4470ac44

#define CRC32_X3359_MODG 0x4b700aa8
#define CRC32_X3295_MODG 0xeea395c4

#define CRC32_X3487_MODG 0xeff5e99d
#define CRC32_X3423_MODG 0xf9d9c7ee

#define CRC32_X3615_MODG 0xad0d2bb2
#define CRC32_X3551_MODG 0xcd669a40

#define CRC32_X3743_MODG 0x9fb66bd3
#define CRC32_X3679_MODG 0x6d40f445

#define CRC32_X3871_MODG 0xc2dcc467
#define CRC32_X3807_MODG 0x9ee62949

#define CRC32_X3999_MODG 0x398e2ff2
#define CRC32_X3935_MODG 0x145575d5

#define CRC32_X4127_MODG 0x1072db28
#define CRC32_X4063_MODG 0x0c30f51d

#define CRC32_BARRETT_CONSTANT_1 0xb4e5b025f7011641ULL

#define CRC32_BARRETT_CONSTANT_2 0x00000001db710641ULL

#define CRC32_NUM_CHUNKS 4
#define CRC32_MIN_VARIABLE_CHUNK_LEN 128UL
#define CRC32_MAX_VARIABLE_CHUNK_LEN 16384UL

static const u32 crc32_mults_for_chunklen[][CRC32_NUM_CHUNKS - 1] =

    {
        {0},

        {
            0xd31343ea,
            0xe95c1271,
            0x910eeec1,
        },

        {
            0x1d6708a0,
            0x0c30f51d,
            0xe95c1271,
        },

        {
            0xdb3839f3,
            0x1d6708a0,
            0xd31343ea,
        },

        {
            0x1753ab84,
            0xbbf2f6d6,
            0x0c30f51d,
        },

        {
            0x3796455c,
            0xb8e0e4a8,
            0xc352f6de,
        },

        {
            0x3954de39,
            0x1753ab84,
            0x1d6708a0,
        },

        {
            0x632d78c5,
            0x3fc33de4,
            0x9a1b53c8,
        },

        {
            0xa0decef3,
            0x7b4aa8b7,
            0xbbf2f6d6,
        },

        {
            0xe9c09bb0,
            0x3954de39,
            0xdb3839f3,
        },

        {
            0xd51917a4,
            0xcae68461,
            0xb8e0e4a8,
        },

        {
            0x154a8a62,
            0x41e7589c,
            0x3e9a43cd,
        },

        {
            0xf196555d,
            0xa0decef3,
            0x1753ab84,
        },

        {
            0x8eec2999,
            0xefb0a128,
            0x6044fbb0,
        },

        {
            0x27892abf,
            0x48d72bb1,
            0x3fc33de4,
        },

        {
            0x77bc2419,
            0xd51917a4,
            0x3796455c,
        },

        {
            0xcea114a5,
            0x68c0a2c5,
            0x7b4aa8b7,
        },

        {
            0xa1077e85,
            0x188cc628,
            0x0c21f835,
        },

        {
            0xc5ed75e1,
            0xf196555d,
            0x3954de39,
        },

        {
            0xca4fba3f,
            0x0acfa26f,
            0x6cb21510,
        },

        {
            0xcf5bcdc4,
            0x4fae7fc0,
            0xcae68461,
        },

        {
            0xf36b9d16,
            0x27892abf,
            0x632d78c5,
        },

        {
            0xf76fd988,
            0xed5c39b1,
            0x41e7589c,
        },

        {
            0x6c45d92e,
            0xff809fcd,
            0x0c46baec,
        },

        {
            0x6116b82b,
            0xcea114a5,
            0xa0decef3,
        },

        {
            0x4d9899bb,
            0x9f9d8d9c,
            0x53deb236,
        },

        {
            0x3e7c93b9,
            0x6666b805,
            0xefb0a128,
        },

        {
            0x388b20ac,
            0xc5ed75e1,
            0xe9c09bb0,
        },

        {
            0x0956d953,
            0x97fbdb14,
            0x48d72bb1,
        },

        {
            0x55cb4dfe,
            0x1b37c832,
            0xc07331b3,
        },

        {
            0x52222fea,
            0xcf5bcdc4,
            0xd51917a4,
        },

        {
            0x0603989b,
            0xb03c8112,
            0x5e04b9a5,
        },

        {
            0x4470c029,
            0x2339d155,
            0x68c0a2c5,
        },

        {
            0xb6f35093,
            0xf76fd988,
            0x154a8a62,
        },

        {
            0xc46805ba,
            0x416f9449,
            0x188cc628,
        },

        {
            0xc3876592,
            0x4b809189,
            0xc35cf6e7,
        },

        {
            0x5b0c98b9,
            0x6116b82b,
            0xf196555d,
        },

        {
            0x30d13e5f,
            0x4c5a315a,
            0x8c224466,
        },

        {
            0x54afca53,
            0xbccfa2c1,
            0x0acfa26f,
        },

        {
            0x93102436,
            0x3e7c93b9,
            0x8eec2999,
        },

        {
            0xbd2655a8,
            0x3e116c9d,
            0x4fae7fc0,
        },

        {
            0x70cd7f26,
            0x408e57f2,
            0x1691be45,
        },

        {
            0x2d546c53,
            0x0956d953,
            0x27892abf,
        },

        {
            0xb53410a8,
            0x42ebf0ad,
            0x161f3c12,
        },

        {
            0x67a93f75,
            0xcf3233e4,
            0xed5c39b1,
        },

        {
            0x9830ac33,
            0x52222fea,
            0x77bc2419,
        },

        {
            0xb0b6fc3e,
            0x2fde73f8,
            0xff809fcd,
        },

        {
            0x84170f16,
            0xced90d99,
            0x30de0f98,
        },

        {
            0xd7017a0c,
            0x4470c029,
            0xcea114a5,
        },

        {
            0xadb25de6,
            0x84f40beb,
            0x2b7e0e1b,
        },

        {
            0x8282fddc,
            0xec855937,
            0x9f9d8d9c,
        },

        {
            0x46362bee,
            0xc46805ba,
            0xa1077e85,
        },

        {
            0xb9077a01,
            0xdf7a24ac,
            0x6666b805,
        },

        {
            0xf51d9bc6,
            0x2b52dc39,
            0x7e774cf6,
        },

        {
            0x4ca19a29,
            0x5b0c98b9,
            0xc5ed75e1,
        },

        {
            0xdc0fc3fc,
            0xb939fcdf,
            0x3678fed2,
        },

        {
            0x63c3d167,
            0x70f9947d,
            0x97fbdb14,
        },

        {
            0x5851d254,
            0x54afca53,
            0xca4fba3f,
        },

        {
            0xfeacf2a1,
            0x7a3c0a6a,
            0x1b37c832,
        },

        {
            0x93b7edc8,
            0x1fea4d2a,
            0x58fa96ee,
        },

        {
            0x5539e44a,
            0xbd2655a8,
            0xcf5bcdc4,
        },

        {
            0xde32a3d2,
            0x4ff61aa1,
            0x6a6a3694,
        },

        {
            0xf0baeeb6,
            0x7ae2f6f4,
            0xb03c8112,
        },

        {
            0xbe15887f,
            0x2d546c53,
            0xf36b9d16,
        },

        {
            0x64f34a05,
            0xe0ee5efe,
            0x2339d155,
        },

        {
            0x1b6d1aea,
            0xfeafb67c,
            0x4fb001a8,
        },

        {
            0x82adb0b8,
            0x67a93f75,
            0xf76fd988,
        },

        {
            0x694587c7,
            0x3b34408b,
            0xeccb2978,
        },

        {
            0xd2fc57c3,
            0x07fcf8c6,
            0x416f9449,
        },

        {
            0x9dd6837c,
            0xb0b6fc3e,
            0x6c45d92e,
        },

        {
            0x3a9d1f97,
            0xefd033b2,
            0x4b809189,
        },

        {
            0x1eee1d2a,
            0xf2a6e46e,
            0x55b4c814,
        },

        {
            0xb57c7728,
            0xd7017a0c,
            0x6116b82b,
        },

        {
            0xf2fc5d61,
            0x242aac86,
            0x05245cf0,
        },

        {
            0x26387824,
            0xc15c4ca5,
            0x4c5a315a,
        },

        {
            0x8c151e77,
            0x8282fddc,
            0x4d9899bb,
        },

        {
            0x8ea1f680,
            0xf5ff6cdd,
            0xbccfa2c1,
        },

        {
            0xe8cf3d2a,
            0x338b1fb1,
            0xeda61f70,
        },

        {
            0x21f15b59,
            0xb9077a01,
            0x3e7c93b9,
        },

        {
            0x6f68d64a,
            0x901b0161,
            0xb9fd3537,
        },

        {
            0x71b74d95,
            0xf5ddd5ad,
            0x3e116c9d,
        },

        {
            0x4c2e7261,
            0x4ca19a29,
            0x388b20ac,
        },

        {
            0x8a2d38e8,
            0xd27ee0a1,
            0x408e57f2,
        },

        {
            0x7e58ca17,
            0x69dfedd2,
            0x3a76805e,
        },

        {
            0xf997967f,
            0x63c3d167,
            0x0956d953,
        },

        {
            0x48215963,
            0x71e1dfe0,
            0x42a6d410,
        },

        {
            0xa704b94c,
            0x679f198a,
            0x42ebf0ad,
        },

        {
            0x1d699056,
            0xfeacf2a1,
            0x55cb4dfe,
        },

        {
            0x6800bcc5,
            0x16024f15,
            0xcf3233e4,
        },

        {
            0x2d48e4ca,
            0xbe61582f,
            0x46026283,
        },

        {
            0x4c4c2b55,
            0x5539e44a,
            0x52222fea,
        },

        {
            0xd8ce94cb,
            0xbc613c26,
            0x33776b4b,
        },

        {
            0xd0b5a02b,
            0x490d3cc6,
            0x2fde73f8,
        },

        {
            0xa223f7ec,
            0xf0baeeb6,
            0x0603989b,
        },

        {
            0x58de337a,
            0x3bf3d597,
            0xced90d99,
        },

        {
            0x37f5d8f4,
            0x4d5b699b,
            0xd7262e5f,
        },

        {
            0xfa8a435d,
            0x64f34a05,
            0x4470c029,
        },

        {
            0x238709fe,
            0x52e7458f,
            0x9a174cd3,
        },

        {
            0x9e1ba6f5,
            0xef0272f7,
            0x84f40beb,
        },

        {
            0xcd8b57fa,
            0x82adb0b8,
            0xb6f35093,
        },

        {
            0x0aed142f,
            0xb1650290,
            0xec855937,
        },

        {
            0xd1f064db,
            0x6e7340d3,
            0x5c28cb52,
        },

        {
            0x464ac895,
            0xd2fc57c3,
            0xc46805ba,
        },

        {
            0xa0e6beea,
            0xcfeec3d0,
            0x0225d214,
        },

        {
            0x78703ce0,
            0xc60f6075,
            0xdf7a24ac,
        },

        {
            0xfea48165,
            0x3a9d1f97,
            0xc3876592,
        },

        {
            0xdb89b8db,
            0xa6172211,
            0x2b52dc39,
        },

        {
            0x7ca03731,
            0x1db42849,
            0xc5df246e,
        },

        {
            0x8801d0aa,
            0xb57c7728,
            0x5b0c98b9,
        },

        {
            0xf89cd7f0,
            0xcc396a0b,
            0xdb799c51,
        },

        {
            0x1611a808,
            0xaeae6105,
            0xb939fcdf,
        },

        {
            0xe3cdb888,
            0x26387824,
            0x30d13e5f,
        },

        {
            0x552a4cf6,
            0xee2d04bb,
            0x70f9947d,
        },

        {
            0x85e248e9,
            0x0a79663f,
            0x53339cf7,
        },

        {
            0x1c61c3e9,
            0x8ea1f680,
            0x54afca53,
        },

        {
            0xb14cfc2b,
            0x2e073302,
            0x10897992,
        },

        {
            0x6ec444cc,
            0x9e819f13,
            0x7a3c0a6a,
        },

        {
            0xe2fa5f80,
            0x21f15b59,
            0x93102436,
        },

        {
            0x6d33f4c6,
            0x31a27455,
            0x1fea4d2a,
        },

        {
            0xb6dec609,
            0x4d437056,
            0x42eb1e2a,
        },

        {
            0x1846c518,
            0x71b74d95,
            0xbd2655a8,
        },

        {
            0x9f947f8a,
            0x2b501619,
            0xa4924b0e,
        },

        {
            0xb7442f4d,
            0xba30a5d8,
            0x4ff61aa1,
        },

        {
            0xe2c93242,
            0x8a2d38e8,
            0x70cd7f26,
        },

        {
            0xcd6863df,
            0x78fd88dc,
            0x7ae2f6f4,
        },

        {
            0xd512001d,
            0xe6612dff,
            0x5c4d0ca9,
        },

        {
            0x4e8d6b6c,
            0xf997967f,
            0x2d546c53,
        },

        {
            0xfa653ba1,
            0xc99014d4,
            0xa0c9fd27,
        },

        {
            0x49893408,
            0x29c2448b,
            0xe0ee5efe,
        },
};

#define CRC32_FIXED_CHUNK_LEN 32768UL
#define CRC32_FIXED_CHUNK_MULT_1 0x29c2448b
#define CRC32_FIXED_CHUNK_MULT_2 0x4b912f53
#define CRC32_FIXED_CHUNK_MULT_3 0x454c93be

#endif
