#include "countries.h"

namespace {

struct Entry {
    uint8_t ecc;
    uint8_t id;      // country id (top nibble of EId / SId)
    Country c;
};

// ETSI TS 101 756 table 3 (European broadcasting area), plus Australia (table 6)
const Entry kTable[] = {
    {0xE0, 0x1, {"de", "Germany"}},       {0xE0, 0x2, {"dz", "Algeria"}},     {0xE0, 0x3, {"ad", "Andorra"}},
    {0xE0, 0x4, {"il", "Israel"}},        {0xE0, 0x5, {"it", "Italy"}},       {0xE0, 0x6, {"be", "Belgium"}},
    {0xE0, 0x7, {"ru", "Russia"}},        {0xE0, 0x8, {"ps", "Palestine"}},   {0xE0, 0x9, {"al", "Albania"}},
    {0xE0, 0xA, {"at", "Austria"}},       {0xE0, 0xB, {"hu", "Hungary"}},     {0xE0, 0xC, {"mt", "Malta"}},
    {0xE0, 0xD, {"de", "Germany"}},       {0xE0, 0xE, {"es", "Canary Islands (Spain)"}},
    {0xE0, 0xF, {"eg", "Egypt"}},

    {0xE1, 0x1, {"gr", "Greece"}},        {0xE1, 0x2, {"cy", "Cyprus"}},      {0xE1, 0x3, {"sm", "San Marino"}},
    {0xE1, 0x4, {"ch", "Switzerland"}},   {0xE1, 0x5, {"jo", "Jordan"}},      {0xE1, 0x6, {"fi", "Finland"}},
    {0xE1, 0x7, {"lu", "Luxembourg"}},    {0xE1, 0x8, {"bg", "Bulgaria"}},    {0xE1, 0x9, {"dk", "Denmark"}},
    {0xE1, 0xA, {"gi", "Gibraltar"}},     {0xE1, 0xB, {"iq", "Iraq"}},        {0xE1, 0xC, {"gb", "United Kingdom"}},
    {0xE1, 0xD, {"ly", "Libya"}},         {0xE1, 0xE, {"ro", "Romania"}},     {0xE1, 0xF, {"fr", "France"}},

    {0xE2, 0x1, {"ma", "Morocco"}},       {0xE2, 0x2, {"cz", "Czechia"}},     {0xE2, 0x3, {"pl", "Poland"}},
    {0xE2, 0x4, {"va", "Vatican"}},       {0xE2, 0x5, {"sk", "Slovakia"}},    {0xE2, 0x6, {"sy", "Syria"}},
    {0xE2, 0x7, {"tn", "Tunisia"}},       {0xE2, 0x9, {"li", "Liechtenstein"}}, {0xE2, 0xA, {"is", "Iceland"}},
    {0xE2, 0xB, {"mc", "Monaco"}},        {0xE2, 0xC, {"lt", "Lithuania"}},   {0xE2, 0xD, {"rs", "Serbia"}},
    {0xE2, 0xE, {"es", "Spain"}},         {0xE2, 0xF, {"no", "Norway"}},

    {0xE3, 0x1, {"me", "Montenegro"}},    {0xE3, 0x2, {"ie", "Ireland"}},     {0xE3, 0x3, {"tr", "Turkey"}},
    {0xE3, 0x4, {"mk", "North Macedonia"}}, {0xE3, 0x8, {"nl", "Netherlands"}}, {0xE3, 0x9, {"lv", "Latvia"}},
    {0xE3, 0xA, {"lb", "Lebanon"}},       {0xE3, 0xB, {"az", "Azerbaijan"}},  {0xE3, 0xC, {"hr", "Croatia"}},
    {0xE3, 0xD, {"kz", "Kazakhstan"}},    {0xE3, 0xE, {"se", "Sweden"}},      {0xE3, 0xF, {"by", "Belarus"}},

    {0xE4, 0x1, {"md", "Moldova"}},       {0xE4, 0x2, {"ee", "Estonia"}},     {0xE4, 0x3, {"kg", "Kyrgyzstan"}},
    {0xE4, 0x6, {"ua", "Ukraine"}},       {0xE4, 0x7, {"xk", "Kosovo"}},      {0xE4, 0x8, {"pt", "Portugal"}},
    {0xE4, 0x9, {"si", "Slovenia"}},      {0xE4, 0xA, {"am", "Armenia"}},     {0xE4, 0xB, {"uz", "Uzbekistan"}},
    {0xE4, 0xC, {"ge", "Georgia"}},       {0xE4, 0xE, {"tm", "Turkmenistan"}}, {0xE4, 0xF, {"ba", "Bosnia and Herzegovina"}},

    {0xF0, 0x1, {"au", "Australia"}},     {0xF0, 0x2, {"au", "Australia"}},   {0xF0, 0x3, {"au", "Australia"}},
    {0xF0, 0x4, {"au", "Australia"}},     {0xF0, 0x5, {"au", "Australia"}},   {0xF0, 0x6, {"au", "Australia"}},
    {0xF0, 0x7, {"au", "Australia"}},     {0xF0, 0x8, {"au", "Australia"}},
};

} // namespace

const Country *dabCountry(uint8_t ecc, uint16_t eid)
{
    const uint8_t id = (uint8_t)(eid >> 12);
    for (const auto &e : kTable)
        if (e.ecc == ecc && e.id == id) return &e.c;
    return nullptr;
}
