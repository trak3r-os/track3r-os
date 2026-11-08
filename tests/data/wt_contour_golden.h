// SPDX-License-Identifier: GPL-3.0-or-later
// OS-CEWTENV W2 golden, made ONCE on the host (g++ 13.3, -O2) from tracker-ce@7fd6c2c's own
// core/source/mtWavetableEnvelopes.cpp (initWavetableEnvelopes + evalWavetableEnvelopeFloat), see the
// procedure in tests/audio_wt_contour_test.cpp's header. Do not edit by hand.
#pragma once

#include <cstdint>

namespace o3os::test::wt_golden {

// CRC-32 (zlib, as formats::crc32) of each of the 256 contours' 256 bytes, in shape order.
inline constexpr std::uint32_t kContourCrc[256] = {
    0xD302D2ADu,     0x273B1C4Bu,     0xE080E825u,     0x00497500u,     0x3666352Bu,     0xAE05E371u,     0x87062469u,     0x77788FD1u,
    0x07957E7Bu,     0xD7F1107Du,     0x8B9BCD87u,     0xB74ED43Au,     0x4FE954E9u,     0xFA34DE77u,     0xEF92AD74u,     0x705A7262u,
    0xD5756CA1u,     0x42C01047u,     0x9C67643Eu,     0x1155F2F3u,     0xC6E43E92u,     0x5983404Fu,     0xECABFE86u,     0xB1B18041u,
    0xEE8A7FCFu,     0x64D5D528u,     0xD13B4B2Au,     0xE48817D1u,     0x5AD5556Cu,     0x666CD249u,     0xF65AE302u,     0x0322AB30u,
    0x4EBE55F1u,     0x9E138C0Du,     0xDA57EB90u,     0x12E3C6A4u,     0xABC53E73u,     0x4EBE55F1u,     0x9E138C0Du,     0xDA57EB90u,
    0x12E3C6A4u,     0xABC53E73u,     0x4EBE55F1u,     0x9E138C0Du,     0xDA57EB90u,     0x12E3C6A4u,     0xABC53E73u,     0x4EBE55F1u,
    0x7AEDC15Bu,     0x84D36D54u,     0xF4EDAFFBu,     0xABB93FD9u,     0x634251F2u,     0xCDA0B906u,     0x646F71EAu,     0xB61FED90u,
    0xDA405DD3u,     0xC1219220u,     0x2280F0D8u,     0xC771EC5Fu,     0x4661803Du,     0x1C970737u,     0xDA405DD3u,     0x1C970737u,
    0xEF122519u,     0x25E68262u,     0xE6EFC16Au,     0x9DCB30DDu,     0x02782D46u,     0x213969DFu,     0xCA6055A0u,     0xBE61AF0Eu,
    0xB9D5EDD9u,     0x4A31E35Cu,     0x35823047u,     0x276DE702u,     0x9AA31E74u,     0x183D1D09u,     0x8FFBCE98u,     0xC603E507u,
    0x5B14270Au,     0xDDB1E2F0u,     0x2E03269Eu,     0xBEC6DDB4u,     0xFC057B71u,     0x75AF0354u,     0x597A38C6u,     0x7A3B21FEu,
    0xDC9A16B3u,     0xBA898872u,     0xF70C7F2Du,     0x71929526u,     0x03D9AAAEu,     0x4A7B8C23u,     0x9B5E3B0Fu,     0x70DD6F18u,
    0xD78D774Eu,     0xBF15D61Bu,     0x47D324F4u,     0xC7B06E55u,     0xDA25BD4Fu,     0xE208BE0Au,     0xD632490Eu,     0x767E25E2u,
    0x6F05E633u,     0x2DBAEBC5u,     0xDEAB26EFu,     0x12CF2E5Du,     0x30AFECEAu,     0x5538F0D4u,     0x83E333A0u,     0xFAF329F6u,
    0x7B883771u,     0x5690BDE3u,     0xF1DC046Du,     0xDD271461u,     0x660C9945u,     0xEE389F71u,     0x1C0CB5EFu,     0x0A13DCB0u,
    0xB49D543Bu,     0xB8843E68u,     0x847622FCu,     0x60203B30u,     0x9A282921u,     0xF3765612u,     0xBAB33E94u,     0xD9BB2F4Du,
    0x620C8BA2u,     0x7F315EC2u,     0xDF6E47A1u,     0xD0C6A44Du,     0xBEDBE277u,     0x810E1B58u,     0xC36BEC90u,     0x1D49387Bu,
    0xEDCB45EEu,     0x79ECF10Fu,     0x767F0A69u,     0x2D6ADD11u,     0x94BC2B0Bu,     0x53A1DB0Du,     0xF7A6DBD5u,     0xEE78CDDCu,
    0xFFC07E25u,     0x6C68D3A4u,     0x512296D1u,     0xC28A3B50u,     0xE8209AF0u,     0x7B883771u,     0x46C27204u,     0xD56ADF85u,
    0xD4514E9Eu,     0x47F9E31Fu,     0x7AB3A66Au,     0xE91B0BEBu,     0xC3B1AA4Bu,     0x501907CAu,     0x6D5342BFu,     0xFEFBEF3Eu,
    0x262D5D84u,     0x51FF0883u,     0x436191CAu,     0x44CD3FBCu,     0x3748DB92u,     0xDC2B17AFu,     0xCB98078Bu,     0x4EE22305u,
    0x41F5AA3Cu,     0x2CABC5A8u,     0x4A3306B7u,     0xB7E9F77Eu,     0xEA4BB12Fu,     0x85AC494Bu,     0x8C90B67Cu,     0xE01BEA6Au,
    0xB43E8C35u,     0x2AE638F5u,     0x09F6E5FAu,     0x6336FF87u,     0xD608942Fu,     0xF8DBD00Bu,     0x703E45C9u,     0x2623EEC0u,
    0xAF7987A9u,     0x8E4CC834u,     0xFC444459u,     0xCBE8513Eu,     0x2F9B3C22u,     0x7B28DC7Bu,     0x812E1DFFu,     0x3B8DB6F7u,
    0xDCCFD170u,     0x5A239BC6u,     0xA28F6978u,     0x5C1CB472u,     0xF45F1CB5u,     0x1B02558Eu,     0x4E4C75FDu,     0x2935B270u,
    0x4CB7DB75u,     0x11CDAC03u,     0xBB7F4F0Au,     0x22BEB2A5u,     0xF5532D59u,     0xF1C7D3B4u,     0xA69C0F2Fu,     0x26D568F0u,
    0xC7B28C6Au,     0x1F3B220Du,     0x3E161B47u,     0x26247856u,     0x13C8647Au,     0xC7DAB95Du,     0x294B4233u,     0xCAFDD88Bu,
    0x0AEDB481u,     0x32C9093Eu,     0x900A46F6u,     0xBEE1D122u,     0x811A4CAFu,     0xDA58C2A7u,     0x238EF183u,     0x110AE35Fu,
    0x55930278u,     0xEE669283u,     0xC9EEC2C4u,     0xB7A3F790u,     0x04BDE5F7u,     0x70254B11u,     0xADCBA021u,     0xA0436B25u,
    0x650C1713u,     0x7FC17131u,     0xD401AC4Fu,     0xECBDC01Eu,     0x0E8D676Fu,     0x1F7F56E3u,     0x7F4E84A5u,     0xF21C50E7u,
    0x6D24DB19u,     0xF2F9F292u,     0x91F869A0u,     0x585AD9A6u,     0x0907D7ECu,     0xC057A5C4u,     0x569C77AAu,     0x9F6CBC71u,
    0x1BC65A02u,     0x95B6FA71u,     0x6716F8B8u,     0x9E350447u,     0x2D882A23u,     0x806186CEu,     0x30F519C3u,     0x56D55434u,
};

// 64 spot evaluations of evalWavetableEnvelopeFloat: shape, bend byte, phase, value.
struct Spot {
  std::uint8_t shape;
  std::uint8_t bend;
  float phase;
  float value;
};
inline constexpr Spot kSpot[64] = {
    {0u, 0x99u, 0.0f, 0.0f},
    {0u, 0xD0u, 0.200000003f, 0.942801535f},
    {0u, 0x04u, 0.5f, 0.862566411f},
    {0u, 0x4Du, 0.800000012f, 0.173796669f},
    {0u, 0x77u, 1.0f, 0.0f},
    {0u, 0x94u, 0.330000013f, 0.988468349f},
    {0u, 0x47u, 0.660000026f, 0.845743299f},
    {0u, 0x09u, 0.99000001f, 0.901361883f},
    {17u, 0x99u, 0.0f, 1.0f},
    {17u, 0xD0u, 0.200000003f, 0.342000544f},
    {17u, 0x04u, 0.5f, 0.999964774f},
    {17u, 0x4Du, 0.800000012f, 0.157459795f},
    {17u, 0x77u, 1.0f, 0.0f},
    {17u, 0x94u, 0.330000013f, 0.365182608f},
    {17u, 0x47u, 0.660000026f, 0.887480795f},
    {17u, 0x09u, 0.99000001f, 0.968605697f},
    {47u, 0x99u, 0.0f, 1.0f},
    {47u, 0xD0u, 0.200000003f, 0.691234052f},
    {47u, 0x04u, 0.5f, 0.0605773889f},
    {47u, 0x4Du, 0.800000012f, 0.189183652f},
    {47u, 0x77u, 1.0f, 0.0f},
    {47u, 0x94u, 0.330000013f, 0.643633246f},
    {47u, 0x47u, 0.660000026f, 0.883624315f},
    {47u, 0x09u, 0.99000001f, 0.957003474f},
    {63u, 0x99u, 0.0f, 0.0f},
    {63u, 0xD0u, 0.200000003f, 0.0f},
    {63u, 0x04u, 0.5f, 0.722222209f},
    {63u, 0x4Du, 0.800000012f, 0.0f},
    {63u, 0x77u, 1.0f, 1.0f},
    {63u, 0x94u, 0.330000013f, 0.599689782f},
    {63u, 0x47u, 0.660000026f, 0.0153548513f},
    {63u, 0x09u, 0.99000001f, 1.0f},
    {130u, 0x99u, 0.0f, 1.0f},
    {130u, 0xD0u, 0.200000003f, 0.422054082f},
    {130u, 0x04u, 0.5f, 0.58286196f},
    {130u, 0x4Du, 0.800000012f, 0.21285978f},
    {130u, 0x77u, 1.0f, 0.973797858f},
    {130u, 0x94u, 0.330000013f, 0.897070587f},
    {130u, 0x47u, 0.660000026f, 0.879371464f},
    {130u, 0x09u, 0.99000001f, 0.295377016f},
    {171u, 0x99u, 0.0f, 1.0f},
    {171u, 0xD0u, 0.200000003f, 6.44279207e-05f},
    {171u, 0x04u, 0.5f, 0.15165551f},
    {171u, 0x4Du, 0.800000012f, 0.928372622f},
    {171u, 0x77u, 1.0f, 0.0f},
    {171u, 0x94u, 0.330000013f, 0.0f},
    {171u, 0x47u, 0.660000026f, 0.935674608f},
    {171u, 0x09u, 0.99000001f, 0.875004172f},
    {222u, 0x99u, 0.0f, 0.200018287f},
    {222u, 0xD0u, 0.200000003f, 0.554948747f},
    {222u, 0x04u, 0.5f, 0.153934985f},
    {222u, 0x4Du, 0.800000012f, 0.0034012103f},
    {222u, 0x77u, 1.0f, 0.0414129905f},
    {222u, 0x94u, 0.330000013f, 0.850098133f},
    {222u, 0x47u, 0.660000026f, 0.0699799359f},
    {222u, 0x09u, 0.99000001f, 0.995106995f},
    {255u, 0x99u, 0.0f, 1.0f},
    {255u, 0xD0u, 0.200000003f, 0.0868285447f},
    {255u, 0x04u, 0.5f, 0.603416204f},
    {255u, 0x4Du, 0.800000012f, 0.682388127f},
    {255u, 0x77u, 1.0f, 1.0f},
    {255u, 0x94u, 0.330000013f, 0.110199325f},
    {255u, 0x47u, 0.660000026f, 0.945816457f},
    {255u, 0x09u, 0.99000001f, 0.0245408416f},
};

}  // namespace o3os::test::wt_golden
