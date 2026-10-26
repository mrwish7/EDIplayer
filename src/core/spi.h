#pragma once

// SPI (Service and Programme Information, ETSI TS 102 818 / TS 102 371)
// received over DAB packet mode:
//
//   packet mode sub-channel ─► packets (EN 300 401 §5.3.2) ─► MSC data groups
//     ─► MOT directory carousel (EN 301 234) ─► files ─► EDIplayer_SPI/<ECC><EId>/
//                                                 └─► binary SI (TS 102 371) ─► service logos
//
// Logos are taken from each service's <multimedia> elements in the Service
// Information: the 32x32 colour square and the 320x240 image. The logo
// assignment is also written to logos.txt in the output folder, so the next
// time the ensemble is received the logos appear without waiting for the
// carousel.
//
// The programme information (EPG schedules) is saved but not interpreted yet.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using SharedBytes = std::shared_ptr<const Bytes>;

struct ServiceLogos {
    SharedBytes logo32;    // 32 x 32 (or the nearest square, e.g. 128 x 128, if no 32 x 32 is sent)
    SharedBytes logo320;   // 320 x 240
};

struct SpiProgress {
    int carousels = 0;     // SPI components being decoded
    int objects = 0;       // objects listed in the MOT directories
    int complete = 0;      // of which received
    bool directory = false;// at least one directory received
};

// Image dimensions from a PNG / JPEG / GIF / BMP header (false if unknown)
bool imageSize(const uint8_t *data, size_t len, int &w, int &h);

class MotCarousel;

class SpiDecoder {
public:
    SpiDecoder();
    ~SpiDecoder();

    // Where EDIplayer_SPI/ is created (normally the executable's folder)
    void setBaseDir(const std::string &dir) { baseDir_ = dir; }

    // Ensemble identity: selects (and loads) EDIplayer_SPI/<ECC><EId>/
    void setEnsemble(uint8_t ecc, uint16_t eid);

    // Decode packet address `address` of sub-channel `subch` as SPI
    void addComponent(uint8_t subch, uint16_t address);
    bool hasSubchannel(uint8_t subch) const;

    // One logical frame of a packet mode sub-channel
    void feed(uint8_t subch, const uint8_t *mst, size_t len);

    // Logos changed since the last call? `ensemble` is the ensemble's logo
    // (32x32 or nearest square), if it has one.
    bool takeLogos(std::map<uint16_t, ServiceLogos> &out, SharedBytes &ensemble);
    SpiProgress progress() const;
    std::string folder() const { return folder_; }

private:
    struct Packets;
    struct SiLogo {
        std::string url;
        int type = 0, w = 0, h = 0;
    };

    void onObject(const std::string &name, int ctype, int csub, bool compressed, const Bytes &body);
    void parseServiceInformation(const Bytes &body);
    void resolveLogos();
    void saveFile(const std::string &name, const Bytes &body);
    void writeIndex();
    void loadIndex();

    std::string baseDir_, folder_;
    bool haveEnsemble_ = false;
    std::map<uint8_t, std::unique_ptr<Packets>> subch_;                       // packet assembly per sub-channel
    std::map<std::pair<uint8_t, uint16_t>, std::unique_ptr<MotCarousel>> carousels_;
    std::map<std::string, SharedBytes> images_;                               // received images by ContentName
    std::map<uint16_t, std::vector<SiLogo>> siLogos_;                         // SId -> multimedia from SI
    std::vector<SiLogo> siEnsemble_;                                          // ensemble multimedia from SI
    SharedBytes ensembleLogo_;
    std::string ensembleLogoName_;
    std::map<uint16_t, ServiceLogos> logos_;
    std::map<uint16_t, std::pair<std::string, std::string>> logoNames_;       // for logos.txt
    std::vector<std::pair<std::string, Bytes>> pendingSave_;                  // until the folder is known
    std::map<std::string, size_t> saved_;                                     // name -> size written
    bool logosChanged_ = false;
};
