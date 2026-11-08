// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test vectors of the pattern-edit tests (pattern_edit_rules_test.cpp, pattern_edit_app_test.cpp, pad_board_test.cpp).
//
// Expected values: output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors. Each digest below is the FNV-1a (64 bit)
// of the results the reference model gave over one group of inputs (support/step_vectors.h says how a result is reduced to bytes and in which
// order a sweep feeds them; the loops of the tests are the inputs). The cases written out in full are the same facts in readable form.
// A group that fails names the group, and the explicit cases say which rule to look at first.
// Whether the official firmware agrees with them is an open research question.

#include <cstdint>

namespace o3os::test::vectors {

// one group per vectors' note -4..127: change and set note on a single step, every instrument, last-used slot and delta
inline constexpr std::uint64_t kNoteDigest[132] = {
    0x40aeab60a4d3bb0dull, 0x12be0fdb03593e8dull, 0xb00a696cc50e9161ull, 0x0667c08b3a57fff5ull,
    0x30ebe92c547c1eb7ull, 0xe21d256285fe22bcull, 0xaa1d17baaf183668ull, 0x76c0b4983cc8a2a4ull,
    0x0e0c39910a6b5978ull, 0xb6448df23fa5d1e4ull, 0xdb452c45cf931f58ull, 0xe406db15b5ace3acull,
    0x9e4b31402e479a08ull, 0x48d9e8647f4558bcull, 0xc7ac91d245ce8548ull, 0xbd1be0cae95cdf34ull,
    0x48e14584feb86948ull, 0xbfcd4b91d9b247d4ull, 0xbf10e1e13356c2d8ull, 0x8173acc1b4920b6cull,
    0x3cde9203f8332258ull, 0x2166ef28a8c3cedcull, 0x8bc82de2352f7948ull, 0xf7248387d18ca8c4ull,
    0x3296e17cc848acf8ull, 0x5d2b1dbc7c6111a4ull, 0x4147791d12dd08b8ull, 0x2eb7bedbaf80c42cull,
    0x9b6ca7593b389a88ull, 0xaf9c79ba19e1029cull, 0x987ad25c921674e8ull, 0x431145a3438451b4ull,
    0x6dc9af1efbfd3f28ull, 0xbc1ff932ceba85f4ull, 0x2b4bf09e2100e138ull, 0x1d511a9a86c4256cull,
    0xfc8563e12a777938ull, 0xdf310f154da25dfcull, 0x2757ac28bf001768ull, 0xb03a890ff105a4a4ull,
    0x6b1ac2c5aadff478ull, 0x33497ca2ad6dfc24ull, 0x5ac19bbdc4e3ac98ull, 0x9b43f1cf1838236cull,
    0x996bd41d86c27b88ull, 0x8dac78df2f514fbcull, 0xc7274df90aa92508ull, 0xa6738d6a58f2bf74ull,
    0x714847ad27b66148ull, 0x8c9665f6b85743d4ull, 0xd59ace0f3a6e6c58ull, 0x9a09fc404bad546cull,
    0x6c72669e8f794018ull, 0x06e756c47388be5cull, 0xe3609d147f3d8d08ull, 0x67ef8eabbf66d884ull,
    0x090c075149597ff8ull, 0x3419e414d97f19e4ull, 0x41d879a06ea89838ull, 0x5c07d9dc2a49716cull,
    0x5b8c5b5919a95708ull, 0x36fc7fedb02a415cull, 0x0b0aa049c45932e8ull, 0x5169f5c9571e17f4ull,
    0x7ba4da665c0ef8e8ull, 0x0559377a305d78b4ull, 0x891923c2ef696bf8ull, 0x125e9708c4d939ecull,
    0x04ac584143203b38ull, 0xce38b8571d58f03cull, 0xb13bcb9e422078e8ull, 0xea622445a251cb24ull,
    0xb2113f4ff4dfd678ull, 0x20c8a65f580d2864ull, 0xa8f3e4cdb17f9a58ull, 0x4d6d6dcd1ecdd62cull,
    0xd0a6776563752108ull, 0xf3f07ed479a686bcull, 0xc50e606dd17d7d48ull, 0x9157b5e44c4b76b4ull,
    0x002a5d68e450ff48ull, 0x0c8147f9627b1654ull, 0x95614a07fe7cbcd8ull, 0x1962bf77f0e9f0ecull,
    0x3ed4268ec5e276d8ull, 0xa1627bac5e894adcull, 0xc4e87302729e6ac8ull, 0xe4d8ab1ced6c1e44ull,
    0x834918ac981cd2f8ull, 0x500c35da967cbc24ull, 0xae35bbe149fa35b8ull, 0xdc92424c8686a92cull,
    0x630a9d938520d408ull, 0xd0aa0e8210b6b39cull, 0x0873a79062648e68ull, 0x168bca8bf8773434ull,
    0xf4b53db48f8d79a8ull, 0x3a629cb852da51f4ull, 0x83b6f3af3df32d38ull, 0xf5bf6cd2d0e76f6cull,
    0x85c486ce9ff56938ull, 0xc91398a5fefb39fcull, 0xf582845f0f631de8ull, 0xd224697f70e47a24ull,
    0xa6df00f68fc60778ull, 0xc1b21d376f182e24ull, 0x60d16b4beddc6418ull, 0x9acb711efc117b6cull,
    0x59e6ac4847d27388ull, 0xb44f52c52def55bcull, 0xde60a1b867f02d88ull, 0x7b35cedb40003374ull,
    0x1e86783c25069a48ull, 0x8393a6dcaa633f54ull, 0x49e597f43187fe58ull, 0x5758094d3262efecull,
    0x627a70391ba39818ull, 0xb36fe9f5ec33245cull, 0xc33c8a300cdda508ull, 0x9bfa0ec19c98e484ull,
    0xf26a19d3cf9997f8ull, 0x502518094f8bcce4ull, 0xe31b6554848ad138ull, 0xdcdf18859936caecull,
    0x601dd4318b60ec88ull, 0x6a3ee89afff971dcull, 0xf6c3525cc5553168ull, 0xb9e4015553784e33ull,
    0x6c0bce1690e5005dull, 0xe070859737a50d7cull, 0x373eafaa013f543cull, 0x5f91eb0d37938b53ull,
};

// one group per instrument 0..65: the instrument reached by deltas -8..8 in the vectors' order (slots 63..48, then 0..47)
inline constexpr std::uint64_t kWalkDigest[66] = {
    0x9ddfee896ae7d4f5ull, 0x0fef31d0ab6714a0ull, 0x4818251ea7ff8f42ull, 0x26429140e7585b96ull,
    0x3b7f13e5c1bac3bcull, 0x6d4f772d03aa8e44ull, 0xd82d6a32f59bf79full, 0xe7dd9d7751422386ull,
    0xfc1db8b3cb65cc39ull, 0xac5a823713da1e20ull, 0x48b091def1d1055bull, 0x2eb6b72f204ed142ull,
    0x9e942e91e7d1c705ull, 0xc1a5150d3a51e0ccull, 0x91578ed58b52e027ull, 0x54c04ee0a95443eeull,
    0x605e3a2abfe4c4e1ull, 0xbd7ef71be5de4368ull, 0x7fcc7a6abb092c03ull, 0xc3782a25a1f69d8aull,
    0xbd9325912d1d13cdull, 0x153f21fedd12a6f4ull, 0xd4d23478eef52cefull, 0xbedf778aefd0d116ull,
    0xf8c282f9c4bf0189ull, 0x835c5c4ab268cbb0ull, 0x45555c24eb2a3aabull, 0x6c9a207c9fe87352ull,
    0xec9b2f16fdd67c55ull, 0xd189653516f8085cull, 0x4e031948b30f7977ull, 0xd10753d3081e80beull,
    0x5ff2fd81ed53e971ull, 0xe8e1b06d98c316b8ull, 0xc51a548cbc3d2c93ull, 0x258a256ae248481aull,
    0x288f5296d367011dull, 0xda5ec39f1cb36744ull, 0x557b136b8f1ea23full, 0x270b548acb9ddf66ull,
    0xe03459e198f2e6d9ull, 0x487bfdeaf2098c00ull, 0xc5fe3b178b53affbull, 0x6de46e429aaa8d22ull,
    0xa15f462f2cc4dd85ull, 0x99b6e0c97c32e93dull, 0xbd1109cee06259c4ull, 0xe02f7400aa595f6eull,
    0x35bd2d7b5b7ff985ull, 0x1b1fddcec49b6d90ull, 0xbc439141e049a9f2ull, 0xed0113922c929da6ull,
    0x092645b1f9f9288cull, 0xef0b8461e7b5bbf4ull, 0x6201c43eac56a88full, 0xe24ccab005f8d976ull,
    0x67361a956f4d01a9ull, 0x127ed16c46f12e90ull, 0x6ee13d7951233b4bull, 0xa92263abd5792572ull,
    0xa772a734b871f815ull, 0x44b0cea6be3013cdull, 0xf1d3d02232232ab4ull, 0x07f2c0ff1dd1eddeull,
    0x9ddfee896ae7d4f5ull, 0x9ddfee896ae7d4f5ull,
};

// one group per note of the instrument sweep (-4..3, then every 9th): change and set instrument, single and range-started
inline constexpr std::uint64_t kInstrDigest[22] = {
    0xb4722672a7adf395ull, 0x9a409f9a5b8d5725ull, 0x83d87c4d113584c5ull, 0x1e95e12e6cb0c0bdull,
    0xf8713e0307f14631ull, 0x300581fbbbf26705ull, 0x5bfc904f3f020885ull, 0x1932ec0e847a0bd9ull,
    0x57a65a6362f8abf9ull, 0x63dabb97d0ad81a5ull, 0x206f947f6c53a6c5ull, 0x793c95dba2d87b81ull,
    0xaad84940d2004ed1ull, 0x8e88455197cb4085ull, 0x49e2ec61bbe50d55ull, 0x225a535b7fa0e5d9ull,
    0x559861b63cbfa349ull, 0x1599da0326f335a5ull, 0x761bdea1af68ad95ull, 0xe8f0060be4fc2111ull,
    0x52e2a6d35b629151ull, 0x595b872fc6c5eb65ull,
};

// one group per FX type tried (kTypes in the test): value delta, pad, type delta and type set over every stored value, column and session
// (OS-CECHANCE C2 re-pinned: Chance's row reaches 174, its coarse walk and the pads' 0..100 cap change the groups that touch type 4)
inline constexpr std::uint64_t kFxDigest[26] = {
    0xb85f4fd8094556a5ull, 0xce01e8fa8006e5d1ull, 0xfe2fd1d2516f28ddull, 0x6285fb52ea35f825ull,
    0x166c400108ef3a45ull, 0xa36f7fbd84cd4f05ull, 0xb88f7689a58b5019ull, 0x79f144cc51b091b5ull,
    0xf8fc60a8ffe0a3edull, 0xb62a338ae4192a11ull, 0x134657ffe4ab2295ull, 0xcd7da224e1aafbd9ull,
    0xa78f35b00ac6da59ull, 0x3b8a91c6f957ba51ull, 0xefabab03fc068c99ull, 0xce39ebb3fd79e7ddull,
    0x3e4f6a1496e1dacdull, 0x5d86c6d51b5b18bdull, 0x3466b8900b6f1eb5ull, 0x70a175c5cc49de95ull,
    0xda0333aa1550eae1ull, 0x80419fa04ad43121ull, 0x3ff2f4ddc5e3cfa1ull, 0x4b4ea9b5b5b97d21ull,
    0xf2f89b49ad5c1ae1ull, 0x47bd396ebc8244a1ull,
};

// one group per element set cleared (all, notes, instruments, FX1, FX2) over the kNotes steps
inline constexpr std::uint64_t kClearDigest[5] = {
    0xcf1b8a402374f21aull, 0x823ee58d3119cd7cull, 0x823ee58d3119cd7cull, 0x1c6fd9d99a03635bull,
    0x91a9b493d0893511ull,
};

// 3000 random selections and operations, 100 to a group, the comparable ones only
// (OS-CECHANCE C2 re-pinned where an episode lands on Chance)
inline constexpr std::uint64_t kRandomDigest[30] = {
    0x03af1e3c538b64f8ull, 0x8d7c3155ec373c77ull, 0x93173132906ae4c5ull, 0x88d449465a40bacfull,
    0x388285c26c66b48eull, 0x0cd3c186af090f04ull, 0x64376ab25f394b63ull, 0xb331857d4cd15f82ull,
    0xcd613b4f3689e919ull, 0x096c3e10fbfd24b2ull, 0x054cf6163126475dull, 0x2a90d419b5989909ull,
    0xde4a5eae2ea0fac5ull, 0xa33a5a9b39153a29ull, 0x79857eac7d1c7a78ull, 0x9eb7c03fc77a9566ull,
    0xfd12df4e10f06f8aull, 0x943b201060d149d8ull, 0xc9abbe4a528efb8full, 0xdb858077ac5e9669ull,
    0x54f9090a4f062d8bull, 0xa2f1bd474f92f1d6ull, 0x511b52bc0274c393ull, 0xbb1a2b435ec0f04bull,
    0x4aad219fdd22feafull, 0x51440982be498e3bull, 0xb81db6014421067aull, 0xc89155044c631cdcull,
    0x6fe490bb8cf28562ull, 0x2638de99d51e81adull,
};

// one group per pattern length 1..128: the row after an entry for every edit step 0..32 shorter than the pattern
inline constexpr std::uint64_t kCursorDigest[128] = {
    0x4d25767f9dce13f5ull, 0xb1d76322112075d5ull, 0xe940d595c512b566ull, 0x9b5f72b72cdd6265ull,
    0x3d21fea7b0dfa971ull, 0x4429da7989e8e8b5ull, 0x28d13b02324a1002ull, 0x413125ac7ae50a25ull,
    0xf67f1a95dc3edd7dull, 0x703df18491876815ull, 0x36391f889881d9aeull, 0x7a277fc7c9a48665ull,
    0xef081814a422a2f9ull, 0x5e314a2a818959f5ull, 0xb66e587c62fbb64aull, 0x73e44677b1926f25ull,
    0xe7d4c5b55b807885ull, 0x82240a8b64008a55ull, 0x3303c6bea5ccd4d6ull, 0x3c8d445ee863c4e5ull,
    0xe6b6153de35d37a1ull, 0x314760d970c2bd35ull, 0xd930636e63f81f52ull, 0xcb5ac3cc53594425ull,
    0x1e6d6a260fff81cdull, 0x87ec93bcb2441495ull, 0x873eca7d6bde655eull, 0xd41ec7ef512478e5ull,
    0x735c4faac05369e9ull, 0x90b79f300918f675ull, 0xa6e84d9d8a343adaull, 0x78d7cffe44b7db25ull,
    0x08809da9246e4ad5ull, 0x8e709116505d80b4ull, 0x27e639760a798c76ull, 0xe99e3d0d487d71a5ull,
    0x0a55aba5b1340891ull, 0x0359b040c02f5074ull, 0xba4f9b2e2bc909f2ull, 0x66304885bc6d6f25ull,
    0xaccf9722bebf91b2ull, 0xcbd9bfca88388032ull, 0x3ecc8fbe6aed737cull, 0xc89add37c3c27ec8ull,
    0xcfab3785cb4634a6ull, 0x750c6e4857751696ull, 0xf7502b2df24b93f8ull, 0xfaa95dad9471821cull,
    0x12f097c40eb5ff5cull, 0xc85bf58027a14e79ull, 0x57277d63fe174ac4ull, 0xda349faeaaa7d1b5ull,
    0xe88fb435ba0a5bfcull, 0xfd8d88ed65c99d99ull, 0x44f91dc27e0991bdull, 0x0236ceef1480de3dull,
    0xdfe7faeb54530951ull, 0x2d31d041f15e8577ull, 0xd7fcbf8e9c9e9cafull, 0x3f702ec790ec4bc9ull,
    0x112300ace9221355ull, 0xb18b368549ef4771ull, 0x0fec1bf8be419cf1ull, 0xc02f58d951a4dc8aull,
    0x5432d82fb1c316a5ull, 0xeaafd79ceb858e86ull, 0x4d0dfac0935f2999ull, 0x69bc4ceeeb6867aaull,
    0x7881304d2c8ef64eull, 0xa4d17b8a4007af8eull, 0x9c8c928fdf62f72cull, 0x435b3eeee6f7f6f4ull,
    0xe8af0c125c00087eull, 0xa89b0aa120734e6aull, 0xd2058fa599848020ull, 0xab57a0437d8b9b84ull,
    0x6932252f97ab0da4ull, 0xdcbbd77d910fc91dull, 0x8d27fc5264ed09fcull, 0xeefe2e0478ccd851ull,
    0xbd4a0ac621f5963cull, 0x56ed55e60d0eb67dull, 0x3e6e3e622aff17d9ull, 0x5ac4f5f2221ff319ull,
    0xfa2352bd0a9f0679ull, 0x0e9bf3c48d1ca2b3ull, 0x187b5d90aa4269a7ull, 0xc9803ce06d0ce08dull,
    0x353dae1dc27bd835ull, 0x55ab0c6e6db4a591ull, 0xa60eef427863c4d1ull, 0x366db9812e80adceull,
    0xb01ff4bacddb121dull, 0x6c21741a31910282ull, 0x438c276bbb5ffe21ull, 0xe6398b965e4d482eull,
    0x132b54925593bf4aull, 0x5562a95c11ee1f4aull, 0x77fea5ba46cf754cull, 0xbe990925160fefb0ull,
    0x9f982b5c339f60b6ull, 0xf98623498889d6eeull, 0x37f3405256f8a708ull, 0xd5561b7f78acc2ecull,
    0xb468976e66f33f4cull, 0xb244c250bf3d3531ull, 0xc8c01ef19f6633d4ull, 0x99b8984ddea9b03dull,
    0x2e05d8ae2196baecull, 0x2a82bf0f4e551d11ull, 0xf3c7891f1192feb5ull, 0x57b05f22f595d3b5ull,
    0x6afe838615dabe31ull, 0x1d833998a281075full, 0x2df59860a1b8520full, 0x10ba3bb7c290c8a1ull,
    0xf77936de56953cf5ull, 0x7f6fb239ede1c651ull, 0x52b66b9d7bde2611ull, 0x48c10bf714429922ull,
    0x6901c0e91da6a9f5ull, 0x6ebada35822b9faeull, 0xc5466d9b7b184d29ull, 0x6afef7f387d40ec2ull,
    0xe476201dceb08de6ull, 0xb403b127154c92e6ull, 0x768924aa045d7d9cull, 0x2e010bc985738f5cull,
};

// one group per scale 0..44 (the corrected rows left out): is the note in the scale, notes 0..255 over roots 0..255
inline constexpr std::uint64_t kScaleDigest[41] = {
    0xdb1f14b358c5df25ull, 0xdb1f14b358c5df25ull, 0x0d2d74c1ef0430d8ull, 0x13c2b20d1758995dull,
    0x6a95b00a491e1ad7ull, 0xf240ca9ed44667f5ull, 0xf4dfad1b0b1912d1ull, 0x335c017f50085715ull,
    0x259e8aec5c14126dull, 0x1961a934fee80df8ull, 0xb9afec373cf2fa16ull, 0xadc88c8eafa041b7ull,
    0x798a0b51c561d295ull, 0x34182140514b5f2full, 0x9fcc7aa61dca28d3ull, 0x0d108640d1198356ull,
    0xf1971a7de0ad893aull, 0x9537270e664d228cull, 0xf80761cdb8240d4bull, 0x26752fc5335390c7ull,
    0x5f6962007b94668cull, 0xfadc7b102ac44325ull, 0x91d8fc574a8624f0ull, 0x71c5bfea2c2a4185ull,
    0xae0e1c89ee97df26ull, 0x273e8bb920dffe80ull, 0xfd8e5868ed965f94ull, 0x8e779de7d39d7198ull,
    0x91d8fc574a8624f0ull, 0x3dc83b1fad368921ull, 0x395b5277de19d49aull, 0xe473dac15485e627ull,
    0x9ec1b4226cab2f9full, 0x83d7c012c8fbfc8dull, 0x2f8b4b28fab81446ull, 0x45fe4b7c6318eaf1ull,
    0x45fe4b7c6318eaf1ull, 0x45fe4b7c6318eaf1ull, 0x45fe4b7c6318eaf1ull, 0x45fe4b7c6318eaf1ull,
    0x45fe4b7c6318eaf1ull,
};

// one group per scale 0..40 (the corrected rows left out): the 48 pad notes for every offset 0..14 and root 0..62
inline constexpr std::uint64_t kPadDigest[37] = {
    0xc5190254c0402115ull, 0xc5190254c0402115ull, 0x9640b1ab6168237bull, 0x30f421011924de3dull,
    0x1b59df206b1aa7c5ull, 0xb3ecaf760be52643ull, 0x243ff7b2ac22bdf4ull, 0xc3d489d1e3d8a901ull,
    0x0d8a71f486e88f07ull, 0x8f027f7945bd1572ull, 0x901bc6c6367283e0ull, 0xba8ed88c8a0373caull,
    0xea942617949f0ca5ull, 0x69a6aca075679f69ull, 0x24e11836d9cee4dcull, 0xbe7745a5b0e848faull,
    0xa81879bdedfefc2aull, 0x960c17a0e0c61768ull, 0xfbe63952ec0cef7cull, 0xfd9cf827ef59174bull,
    0xc1f8610e3e0009efull, 0x7ef6ed28baa3d5cfull, 0xeaee1ed8097eb9adull, 0xaa4512868cafb39eull,
    0xaff8c3f5bfd18d98ull, 0xc5eea0756524c884ull, 0x435658fb828907eeull, 0x80693b4df8690f8dull,
    0xeaee1ed8097eb9adull, 0x14dd78f536ad0bc0ull, 0x5f5190730f0d4742ull, 0xd61f6c24ea1f2513ull,
    0x4ae173864404ebe0ull, 0x1ad71d51ae4487acull, 0x475e21e25d59781eull, 0x34c9b150685d6693ull,
    0x34c9b150685d6693ull,
};

// 60 random patterns of 16 tracks x 128 steps: the pattern after Insert or Backspace over a block, and whether it changed
inline constexpr std::uint64_t kInsertBackspaceDigest[60] = {
    0x09a962699a2c5121ull, 0x9315af72fdf104f7ull, 0x06117eb9f3384094ull, 0x7d4d6efee15b7131ull,
    0x7728f966b7c71587ull, 0xa51dfd1d9bfb5fb2ull, 0xa2e4f8730b0c8685ull, 0xfb120965470afb0aull,
    0x282cae8608a4e65eull, 0x1c0a1d609f15e59eull, 0x74e7acc35c32d1feull, 0x79f5664135a98a26ull,
    0xf0504f486d55e917ull, 0xcdc6eb62dc901ef9ull, 0x6233e03d0240296full, 0xe164c5f88fa5f366ull,
    0x03b7821dbbaf6f5cull, 0x60f93448de26cb47ull, 0x9404ff23c70e34dfull, 0x688929fecfa89dc0ull,
    0x2589dd38aba38819ull, 0xdbd4b8803a0ebb51ull, 0x845913f08f4677f5ull, 0x8abfc7d0a608140dull,
    0x1d4eadcb311de95aull, 0x89fabff4cd52cb6eull, 0x44135b948dc16b69ull, 0x4e8d4e48fdfa2b4cull,
    0xc7350b052fb9a22eull, 0xd77c5a1e934bf45bull, 0x90c8fcc80b80916cull, 0x6930a2e6e4d6c414ull,
    0x26b0595a68dd4514ull, 0xf8484be3488bbcdfull, 0x8a08809b4ee24a41ull, 0x8e45874b980c2cacull,
    0x603f75d695057966ull, 0x9997204ab2a4bc04ull, 0x621046526a25a6c2ull, 0xe6bd0c87b126fe28ull,
    0x2e7a8a001eb62941ull, 0xd78bc8aa66f65434ull, 0xbe66a69a5a90257full, 0x6293b255fbab34c6ull,
    0x61b3694c5a892f64ull, 0xbe977679d6645e2bull, 0x9bc6bb24a42bb11eull, 0x6a49c7d288daad0eull,
    0x16f85ff374c68181ull, 0xdc7020444b75e9b3ull, 0x83e33c5fa8912fc5ull, 0xd34a7288f67aaaebull,
    0xb4a55869c3092c69ull, 0x65bdf8ccfd5ea13dull, 0xfaa904ad73572011ull, 0xd08a650090f55fc6ull,
    0x0167190d65235875ull, 0x56df44f7c5f9f0dfull, 0xd146dbf0ff46b3acull, 0x331fb59c2fd7d0e3ull,
};

// The 40 scale masks of the reference model (bit k = the note k semitones above the root is in the scale). The four corrected rows
// (core::scale_corrected) differ on purpose.
inline constexpr std::uint16_t kScaleMask[40] = {
    0b111111111111, 0b111111111111, 0b010110101101, 0b101010110101,
    0b011010101101, 0b101011010101, 0b010111010101, 0b010101101011,
    0b010110101011, 0b010110110011, 0b011010110101, 0b011010101101,
    0b100110101101, 0b101110110101, 0b011010111101, 0b111010110101,
    0b010011101001, 0b110110101101, 0b010010101001, 0b001010010101,
    0b100111001101, 0b011011001101, 0b101011010011, 0b100011101011,
    0b010101010101, 0b101101101101, 0b010101011011, 0b000110001101,
    0b010010100011, 0b001010100101, 0b010001100011, 0b101101101101,
    0b001010001101, 0b011011010101, 0b100110110011, 0b010110110001,
    0b100110110011, 0b101010101011, 0b100110101011, 0b110101010011,
};

// The note names of the reference model, note 0 = "C0" .. 127 = "G10" (the model's note n is the vectors' n - 1).
inline constexpr const char* kNoteNames[128] = {
    "C0", "C#0", "D0", "D#0", "E0", "F0", "F#0", "G0", "G#0", "A0", "A#0", "B0",
    "C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1",
    "C2", "C#2", "D2", "D#2", "E2", "F2", "F#2", "G2", "G#2", "A2", "A#2", "B2",
    "C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3", "G#3", "A3", "A#3", "B3",
    "C4", "C#4", "D4", "D#4", "E4", "F4", "F#4", "G4", "G#4", "A4", "A#4", "B4",
    "C5", "C#5", "D5", "D#5", "E5", "F5", "F#5", "G5", "G#5", "A5", "A#5", "B5",
    "C6", "C#6", "D6", "D#6", "E6", "F6", "F#6", "G6", "G#6", "A6", "A#6", "B6",
    "C7", "C#7", "D7", "D#7", "E7", "F7", "F#7", "G7", "G#7", "A7", "A#7", "B7",
    "C8", "C#8", "D8", "D#8", "E8", "F8", "F#8", "G8", "G#8", "A8", "A#8", "B8",
    "C9", "C#9", "D9", "D#9", "E9", "F9", "F#9", "G9", "G#9", "A9", "A#9", "B9",
    "C10", "C#10", "D10", "D#10", "E10", "F10", "F#10", "G10",
};

// Single-step cases written out in full. kind 0: one step, no range started; 1: one step, a range started on it; 2: a range over several steps
// (the rule of the step is the multi one). op: n note delta, N note set (arg = the vectors' note), i instrument delta, I instrument set (arg =
// slot 0..63), v FX value delta, p FX value by pad, t FX type delta, T FX type set, c clear (arg: 0 all, 1 notes, 2 instruments, 3 FX1, 4 FX2).
// Input: the vectors' note (-4..127), the model's instrument (0 none, 1..64), the two FX (type, value), the session (last slot, last FX).
// Result: note as a byte (-1 = 255 ...), the vectors' slot, FX types and values, the session.
struct StepCase {
  int kind;
  char op;
  int arg;
  unsigned fx;
  int note;
  unsigned instr;
  unsigned type0, value0, type1, value1;
  unsigned last_slot, last_fx;
  std::uint8_t want[8];
};
inline constexpr StepCase kStepCases[] = {
    {0, 'n', 1, 0, -1, 0, 0, 0, 0, 0, 7, 0, {60, 7, 0, 0, 0, 0, 7, 0}},
    {0, 'n', -1, 0, -1, 0, 0, 0, 0, 0, 7, 0, {254, 7, 0, 0, 0, 0, 7, 0}},
    {0, 'n', 5, 0, 60, 6, 0, 0, 0, 0, 0, 0, {65, 5, 0, 0, 0, 0, 0, 0}},
    {0, 'n', 5, 0, 127, 6, 0, 0, 0, 0, 0, 0, {127, 5, 0, 0, 0, 0, 0, 0}},
    {0, 'n', -5, 0, -3, 0, 0, 0, 0, 0, 0, 0, {252, 0, 0, 0, 0, 0, 0, 0}},
    {2, 'n', -5, 0, 0, 6, 0, 0, 0, 0, 0, 0, {0, 5, 0, 0, 0, 0, 0, 0}},
    {2, 'n', 3, 0, -2, 0, 0, 0, 0, 0, 0, 0, {254, 0, 0, 0, 0, 0, 0, 0}},
    {0, 'N', 60, 0, -1, 0, 0, 0, 0, 0, 7, 0, {60, 7, 0, 0, 0, 0, 7, 0}},
    {0, 'N', -4, 0, 60, 6, 0, 0, 0, 0, 0, 0, {252, 5, 0, 0, 0, 0, 0, 0}},
    {2, 'N', 36, 0, -1, 0, 0, 0, 0, 0, 7, 0, {255, 0, 0, 0, 0, 0, 7, 0}},
    {2, 'N', 36, 0, 60, 6, 0, 0, 0, 0, 0, 0, {36, 5, 0, 0, 0, 0, 0, 0}},
    {0, 'i', -1, 0, 60, 1, 0, 0, 0, 0, 0, 0, {60, 48, 0, 0, 0, 0, 48, 0}},
    {0, 'i', 5, 0, 60, 6, 0, 0, 0, 0, 0, 0, {60, 10, 0, 0, 0, 0, 10, 0}},
    {0, 'i', 1, 0, -1, 0, 0, 0, 0, 0, 9, 0, {60, 9, 0, 0, 0, 0, 9, 0}},
    {2, 'i', -1, 0, -1, 0, 0, 0, 0, 0, 9, 0, {255, 0, 0, 0, 0, 0, 9, 0}},
    {2, 'i', 7, 0, 60, 64, 0, 0, 0, 0, 0, 0, {60, 58, 0, 0, 0, 0, 0, 0}},
    {0, 'I', 4, 0, -1, 0, 0, 0, 0, 0, 0, 0, {60, 4, 0, 0, 0, 0, 4, 0}},
    {2, 'I', 63, 0, 60, 6, 0, 0, 0, 0, 0, 0, {60, 63, 0, 0, 0, 0, 0, 0}},
    {2, 'I', 63, 0, -3, 0, 0, 0, 0, 0, 0, 0, {253, 0, 0, 0, 0, 0, 0, 0}},
    {0, 'I', 20, 0, 50, 2, 0, 0, 0, 0, 3, 0, {50, 20, 0, 0, 0, 0, 20, 0}},
    {0, 'v', 7, 0, 60, 1, 3, 5, 0, 0, 0, 0, {60, 0, 3, 12, 0, 0, 0, 9}},
    {0, 'v', 1, 1, 60, 1, 0, 0, 0, 0, 0, 9, {60, 0, 0, 0, 3, 1, 0, 9}},
    {2, 'v', 1, 1, 60, 1, 0, 0, 0, 0, 0, 9, {60, 0, 0, 0, 0, 0, 0, 0}},
    {0, 'v', -300, 0, 60, 1, 20, 200, 0, 0, 0, 0, {60, 0, 20, 0, 0, 0, 0, 35}},
    {1, 'v', 7, 0, 60, 1, 17, 50, 0, 0, 0, 0, {60, 0, 17, 57, 0, 0, 0, 34}},
    {2, 'v', 300, 1, 60, 1, 0, 0, 18, 100, 0, 0, {60, 0, 0, 0, 18, 100, 0, 2}},
    {0, 'p', 47, 0, 60, 1, 17, 3, 0, 0, 0, 0, {60, 0, 17, 75, 0, 0, 0, 0}},
    {0, 'p', 0, 1, 60, 1, 0, 0, 0, 0, 0, 9, {60, 0, 0, 0, 3, 0, 0, 9}},
    {2, 'p', 24, 0, 60, 1, 21, 1, 0, 0, 0, 0, {60, 0, 21, 17, 0, 0, 0, 0}},
    {0, 't', 1, 0, 60, 1, 2, 40, 0, 0, 0, 0, {60, 0, 4, 40, 0, 0, 0, 0}},
    {0, 't', -1, 0, 60, 1, 13, 1, 0, 0, 0, 0, {60, 0, 37, 1, 0, 0, 0, 0}},
    {0, 't', 1, 1, 60, 1, 0, 0, 0, 0, 0, 0, {60, 0, 0, 0, 0, 0, 0, 0}},
    {0, 't', 1, 0, 60, 1, 17, 90, 0, 0, 0, 0, {60, 0, 20, 90, 0, 0, 0, 0}},
    {0, 'T', 15, 0, 60, 1, 2, 40, 0, 0, 0, 0, {60, 0, 15, 40, 0, 0, 0, 0}},
    {0, 'T', 43, 0, 60, 1, 2, 40, 0, 0, 0, 0, {60, 0, 2, 40, 0, 0, 0, 0}},
    {0, 'T', 3, 1, 60, 1, 0, 0, 17, 90, 0, 0, {60, 0, 0, 0, 3, 53, 0, 0}},
    {0, 'c', 0, 0, 60, 7, 3, 30, 27, 40, 0, 0, {255, 0, 0, 0, 0, 0, 0, 0}},
    {0, 'c', 1, 0, 60, 7, 3, 30, 27, 40, 0, 0, {255, 0, 3, 30, 27, 40, 0, 0}},
    {0, 'c', 2, 0, 60, 7, 3, 30, 27, 40, 0, 0, {255, 0, 3, 30, 27, 40, 0, 0}},
    {0, 'c', 3, 0, 60, 7, 3, 30, 27, 40, 0, 0, {60, 6, 0, 0, 27, 40, 0, 0}},
    {0, 'c', 4, 0, 60, 7, 3, 30, 27, 40, 0, 0, {60, 6, 3, 30, 0, 0, 0, 0}},
};

// Full pad boards (scale, offset, root, then the note of pads 0..47).
struct Board {
  unsigned scale, offset, root;
  std::uint8_t notes[48];
};
inline constexpr Board kBoards[] = {
    {0, 12, 36, {72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47}},
    {3, 12, 36, {98, 100, 101, 103, 105, 107, 108, 110, 112, 113, 115, 117, 77, 79, 81, 83, 84, 86, 88, 89, 91, 93, 95, 96, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55}},
    {3, 0, 36, {36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55, 36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55, 36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55, 36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55}},
    {3, 13, 36, {62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 45, 47, 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55}},
    {24, 12, 62, {120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 110, 112, 114, 116, 118, 120, 120, 120, 120, 120, 120, 120, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84}},
    {9, 13, 20, {45, 48, 49, 51, 52, 54, 56, 57, 60, 61, 63, 64, 37, 39, 40, 42, 44, 45, 48, 49, 51, 52, 54, 56, 28, 30, 32, 33, 36, 37, 39, 40, 42, 44, 45, 48, 20, 21, 24, 25, 27, 28, 30, 32, 33, 36, 37, 39}},
    {16, 5, 59, {89, 90, 93, 95, 98, 100, 101, 102, 105, 107, 110, 112, 78, 81, 83, 86, 88, 89, 90, 93, 95, 98, 100, 101, 69, 71, 74, 76, 77, 78, 81, 83, 86, 88, 89, 90, 59, 62, 64, 65, 66, 69, 71, 74, 76, 77, 78, 81}},
    {39, 7, 0, {36, 37, 40, 42, 44, 46, 47, 48, 49, 52, 54, 56, 24, 25, 28, 30, 32, 34, 35, 36, 37, 40, 42, 44, 12, 13, 16, 18, 20, 22, 23, 24, 25, 28, 30, 32, 0, 1, 4, 6, 8, 10, 11, 12, 13, 16, 18, 20}},
    {40, 14, 12, {37, 40, 42, 44, 46, 47, 48, 49, 52, 54, 56, 58, 30, 32, 34, 35, 36, 37, 40, 42, 44, 46, 47, 48, 22, 23, 24, 25, 28, 30, 32, 34, 35, 36, 37, 40, 12, 13, 16, 18, 20, 22, 23, 24, 25, 28, 30, 32}},
};

}  // namespace o3os::test::vectors
