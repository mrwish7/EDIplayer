#pragma once

// Country of an ensemble from its Extended Country Code (FIG 0/9) and the
// country id in the top nibble of the EId (ETSI TS 101 756, tables 3 to 7).

#include <cstdint>

struct Country {
    const char *iso;    // ISO 3166-1 alpha-2, lower case ("gb")
    const char *name;
};

// nullptr if the combination isn't in the table
const Country *dabCountry(uint8_t ecc, uint16_t eid);
