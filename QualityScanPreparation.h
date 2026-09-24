#pragma once
#include "ScanTelemetry.h"
#include "DiscRotReadConsistency.h"

namespace Diagnostics {
inline constexpr DWORD kQualityIntervalSectors=75;
inline constexpr DWORD kQualityStartupSectors=10*kQualityIntervalSectors;

template<class Disc>
DiscRot::AudioRanges QualityAudioRanges(const Disc& disc) {
    DiscRot::AudioRanges ranges;
    for(const auto& track:disc.tracks) {
        if(!track.isAudio)continue;
        const DWORD first=track.trackNumber==1?0:track.pregapLBA;
        if(track.endLBA>=first)ranges.emplace_back(first,track.endLBA);
    }
    return DiscRot::NormalizeAudioRanges(std::move(ranges));
}

// Count complete observations whose start lies in the startup segment. Raw
// unknown-duration evidence is retained; coverage is never inferred from polls.
struct QualityStartupSummary {
    DWORD firstLba=0,plannedSectors=0;
    std::uint64_t coveredSectors=0,nextLba=0;
    size_t samples=0;
    long long c1=0,secondStage=0,cu=0;
    bool coverageKnown=true;
    void Reset(DWORD first,DWORD sectors) {
        *this=QualityStartupSummary{};firstLba=first;nextLba=first;plannedSectors=sectors;
    }
    bool Contains(DWORD lba) const {
        return plannedSectors>0 && lba>=firstLba && std::uint64_t{lba}<std::uint64_t{firstLba}+plannedSectors;
    }
    void Record(DWORD lba,DWORD sectors,int first,int second,int uncorrectable) {
        if(!Contains(lba))return;
        ++samples;c1+=first;secondStage+=second;cu+=uncorrectable;
        if(sectors==0 || lba!=nextLba || std::uint64_t{lba}+sectors>std::uint64_t{firstLba}+plannedSectors)
            coverageKnown=false;
        else coveredSectors+=sectors;
        nextLba=std::uint64_t{lba}+sectors;
    }
    bool Complete() const {return coverageKnown && coveredSectors==plannedSectors;}
    bool HasActivity() const {return c1>0 || secondStage>0 || cu>0;}
};

template<class Drive>
int QualityScanBufferKB(Drive& drive) {
    std::vector<BYTE> page;
    if(!drive.GetModePage2A(page) || page.size()<16 || (page[0]&0x3F)!=0x2A)return 0;
    return (int(page[12])<<8)|page[13];
}

template<class Drive,class Cancelled>
bool PrepareQualityScanCache(Drive& drive,const DiscRot::AudioRanges& audio,
    DWORD first,DWORD last,int bufferKB,Cancelled cancelled) {
    return DiscRot::EvictAudioCacheRange(first,last,audio,bufferKB,
        [&](DWORD lba,DWORD sectors,BYTE* data) {return drive.ReadSectorsAudioOnly(lba,sectors,data);},cancelled);
}

inline void PrintQualityStartup(std::ostream& out,const QualityStartupSummary& startup,
    const char* secondStage,bool cuMeasured,bool includedInTotals,const char* indent="  ") {
    if(startup.plannedSectors==0)return;
    const auto flags=out.flags();const auto precision=out.precision();out<<std::dec;
    out<<indent<<"Startup segment: LBAs "<<startup.firstLba<<'-'
        <<std::uint64_t{startup.firstLba}+startup.plannedSectors-1<<" ("
        <<std::fixed<<std::setprecision(2)<<startup.plannedSectors/75.0<<" seconds of requested audio).\n";
    if(startup.samples==0)out<<indent<<"Startup observations: NOT MEASURED.\n";
    else {
        out<<indent<<"Startup raw counts: C1 "<<startup.c1<<", "<<secondStage<<' '<<startup.secondStage<<", CU ";
        if(cuMeasured)out<<startup.cu;else out<<"NOT MEASURED";
        out<<"; "<<(startup.Complete()?"complete coverage":"partial or unverified coverage")<<".\n";
    }
    out<<indent<<(includedInTotals?"Startup counts are included in the full-pass totals; none are subtracted.\n"
        :"Startup counts are separate from target totals; positive C2/CU evidence remains a warning.\n");
    out.flags(flags);out.precision(precision);
}
} // namespace Diagnostics
