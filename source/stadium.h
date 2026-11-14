#pragma once
// Stock stadium model IDs from data/maps/interior/stadint.ide and area codes
// from stadint.ipl. Match both: area codes alone are shared by other interiors.
// Text IPL entities need not carry a useful streamed-IPL pool index.
namespace stadium {
inline bool matches(unsigned model, int area) {
    static constexpr unsigned char areas[] = {
        14, 14, 14, 14, 14, 15, 15, 15, // 13590
        1, 1, 1, 1, 1, 255, 255, 16, // 13598
        16, 16, 16, 16, 16, 15, 15, 15, // 13606
        15, 15, 15, 15, 15, 15, 15, 15, // 13614
        16, 16, 7, 7, 7, 1, 7, 7, // 13622
        7, 4, 4, 4, 14, 14, 14, 14, // 13630
        14, 14, 14, 14, 14, 14, 14, 14, // 13638
        14, 14, 14, 14, 14, 15, 15, 15, // 13646
        7, 7, 15, 14, 14, 7, 7, 7, // 13654
        4, 4, 4, 4, 14, 4, // 13662
    };
    return area > 0 && area < 255 && model >= 13590 && model < 13668 &&
           areas[model - 13590] == area;
}
inline bool candidateArea(int area) {
    return area == 1 || area == 4 || area == 7 || area == 14 || area == 15 || area == 16;
}
}
