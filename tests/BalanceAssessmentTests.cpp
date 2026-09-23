#define NOMINMAX
#include "../DiscBalanceAssessment.h"
#include <iostream>
#include <limits>

int RunBalanceAssessmentTests() {
    int failed=0;
    auto check=[&](bool ok,const char* label) { std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n'; if(!ok) ++failed; };
    auto clean=[] {
        std::vector<Diagnostics::BalanceSpeedSample> rows;
        for (int speed : {4,8,16,24,32,40}) {
            Diagnostics::BalanceSpeedSample r;
            r.requestedSpeed=r.actualSpeed=speed;
            r.validReads=50;
            r.readTimeMs=160.0/speed;
            r.stabilityRatio=1;
            r.stabilityMeasured=true;
            rows.push_back(r);
        }
        return rows;
    };
    auto assess=[](const auto& rows) {return Diagnostics::AssessBalance(rows,50,25,false,false);};
    auto rows=clean();
    auto full=assess(rows);
    check(full.available && !full.partial && full.score==100 && full.haveFullScore && full.suggestedSpeed==40,
        "26: a complete verified sweep retains its full range and recommendation");
    for (size_t i=2;i<rows.size();++i) rows[i].actualSpeed=0;
    auto limited=assess(rows);
    check(limited.available && limited.partial && limited.maximumMeasuredSpeed==8 && limited.suggestedSpeed==8 && !limited.haveFullScore,
        "26: only verified 4x/8x produces an 8x limit and no high-speed assessment");
    check(limited.compared==std::vector<bool>({true,true,false,false,false,false}),
        "26: unknown-speed rows are excluded from every scoring input");
    for (size_t i=2;i<rows.size();++i) {
        rows[i].readTimeMs=1000000;
        rows[i].jitterCV=10000;
        rows[i].stabilityRatio=10000;
        rows[i].readErrorSignal=1000000;
        rows[i].validReads=0;
        rows[i].c1Rate=100000;
        rows[i].secondStageRate=100000;
        rows[i].hardwareSamples=15;
    }
    auto polluted=assess(rows);
    check(polluted.score==limited.score && polluted.errorScore==limited.errorScore &&
        polluted.jitterScore==limited.jitterScore && polluted.scalingScore==limited.scalingScore &&
        polluted.stabilityScore==limited.stabilityScore && polluted.suggestedSpeed==limited.suggestedSpeed,
        "26: extreme or failed unknown-speed rows cannot change qualified scores");
    auto noQualifiedHardware=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(!noQualifiedHardware.usingHwEcc && noQualifiedHardware.score==limited.score,
        "26: hardware observations at unknown speeds cannot choose the scoring backend");
    rows=clean();
    rows[2].actualSpeed=0;
    rows[4].actualSpeed=0;
    rows[5].actualSpeed=0;
    auto gap=assess(rows);
    std::vector<Diagnostics::BalanceSpeedSample> compact{rows[0],rows[1],rows[3]};
    auto compactResult=assess(compact);
    check(gap.available && gap.suggestedSpeed==24 && gap.previousRow[3]==1 &&
        gap.score==compactResult.score && gap.scalingScore==compactResult.scalingScore,
        "26: internal unverified gaps are excluded rather than used as adjacent comparisons");
    rows=clean();
    double ms=10;
    for(auto& r:rows) {r.readTimeMs=ms;ms*=2;}
    auto worsening=assess(rows);
    check(worsening.available && worsening.scalingScore==0 && worsening.score<50 && worsening.suggestedSpeed==4 && worsening.timingPenalty.back()==2,
        "26: verified 10-to-320ms regression is retained and limits recommendation to 4x");
    rows=clean();
    for(auto& r:rows) r.readTimeMs=10;
    auto plateau=assess(rows);
    check(plateau.available && plateau.scalingScore<100 && plateau.suggestedSpeed==4,
        "26: a plateau across distinct verified speeds is evidence, not a clamping shortcut");
    rows=clean();
    rows.resize(2); rows[0].readTimeMs=40; rows[1].readTimeMs=39;
    auto nearEqual=assess(rows);
    check(nearEqual.available && nearEqual.scalingScore<100 && nearEqual.suggestedSpeed==4,
        "26: near-equal times at genuinely different speeds do not erase the baseline");
    rows=clean();
    const int clamped[]={4,4,8,8,16,16};
    for(size_t i=0;i<rows.size();++i) { rows[i].actualSpeed=clamped[i];rows[i].readTimeMs=160.0/clamped[i]; }
    auto repeats=assess(rows);
    check(repeats.available && repeats.score==100 && repeats.suggestedSpeed==16 && !repeats.haveFullScore &&
        std::all_of(repeats.timingPenalty.begin(),repeats.timingPenalty.end(),[](int p){return p==0;}) &&
        std::all_of(repeats.primaryCompared.begin(),repeats.primaryCompared.end(),[](bool b){return b;}),
        "26: equivalent same-speed repeats do not invent extra steps or higher recommendations");
    for(auto& r:rows) r.actualSpeed=16;
    check(!assess(rows).available, "26: repeated requests at one actual speed stay unmeasured");
    rows=clean();
    for(size_t i=1;i<rows.size()-1;++i) rows[i].actualSpeed=0;
    rows.back().actualSpeed=8; rows.back().readTimeMs=20;
    auto lowerThanRequest=assess(rows);
    check(lowerThanRequest.available && lowerThanRequest.suggestedSpeed==8,
        "26: requesting 40x but measuring 8x cannot produce a 40x recommendation");
    rows=clean();
    rows[1].validReads=0;rows[1].readTimeMs=0;
    auto readFailure=assess(rows);
    check(readFailure.available && !readFailure.compared[1] && readFailure.errorScore==0 && readFailure.score<=35,
        "26: known-speed read failures still constrain coverage when their timings are unavailable");
    rows=clean();for(auto& r:rows)r.validReads=25;
    check(assess(rows).score<=35, "26: half-readable coverage never becomes a perfect balance score");
    rows=clean();rows[0].readTimeMs=std::numeric_limits<double>::quiet_NaN();
    auto invalid=assess(rows);
    check(invalid.available && !invalid.compared[0] && invalid.score>=0 && invalid.score<=100,
        "26: invalid timing rows cannot contaminate numeric scores");
    rows=clean();rows[0].actualSpeed=8;rows[0].readTimeMs=20;rows[1].actualSpeed=4;rows[1].readTimeMs=40;
    auto reordered=assess(rows);
    check(reordered.available && reordered.score==100 && reordered.previousRow[0]==1,
        "26: actual speed order controls comparisons while report indices map to original rows");
    rows=clean();for(size_t i=2;i<rows.size();++i)rows[i].actualSpeed=0;
    rows[0].hardwareSamples=rows[1].hardwareSamples=15;rows[0].c1Rate=1;rows[1].c1Rate=2;
    auto pioneer=Diagnostics::AssessBalance(rows,50,25,true,true);
    rows[0].secondStageRate=500;rows[1].secondStageRate=50000;
    auto pioneerE22=Diagnostics::AssessBalance(rows,50,25,true,true);
    check(pioneer.usingHwEcc && pioneerE22.score==pioneer.score && pioneerE22.suggestedSpeed==pioneer.suggestedSpeed,
        "26: extracting the scorer preserves Pioneer E22 as diagnostic-only");
    auto reused=limited;
    reused=assess(clean());
    check(reused.available && !reused.partial && reused.haveFullScore && reused.suggestedSpeed==40,
        "26: a new complete sweep clears previous limited-range state");
    rows=clean();for(auto& r:rows)r.actualSpeed=0;reused=assess(rows);
    check(!reused.available && reused.suggestedSpeed==0 && reused.score==0,
        "26: losing measurement coverage clears old scores and speed recommendations");
    return failed;
}

