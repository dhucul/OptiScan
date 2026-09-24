#define NOMINMAX
#include "../DiscBalanceAssessment.h"
#include <iostream>
#include <limits>
#include <sstream>

int RunBalanceAssessmentTests() {
    int failed=0;
    auto check=[&](bool ok,const char* label) { std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n'; if(!ok) ++failed; };
    Diagnostics::BalanceReadRepeats repeats;
    repeats.Record(true,90,12);
    repeats.Record(true,10,0);
    repeats.Record(true,30,6);
    check(repeats.bestMs==10 && repeats.worstMs==90 && repeats.c2Total==18 &&
        repeats.c2PositiveReads==2 && repeats.successfulReads==3 && repeats.AverageC2()==6,
        "26: a faster clean repeat cannot erase C2 from slower successful reads");
    repeats.Record(false,1,999);
    check(repeats.successfulReads==3 && repeats.failedReads==1 && repeats.c2Total==18 && repeats.AverageC2()==6 && repeats.bestMs==10,
        "26: failed transfers do not dilute C2 averages or supply valid counters/timing");
    Diagnostics::BalanceReadRepeats reversed;
    reversed.Record(true,30,6);reversed.Record(true,10,0);reversed.Record(true,90,12);
    check(reversed.c2Total==repeats.c2Total && reversed.AverageC2()==repeats.AverageC2() &&
        reversed.bestMs==repeats.bestMs,
        "26: repeat ordering affects neither retained C2 observations nor fastest timing");
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
    Diagnostics::BalanceReadRepeats recovered;
    recovered.Record(false,1000,999);recovered.Record(true,10,0);recovered.Record(true,10,0);
    Diagnostics::BalanceReadEvidence failures;
    failures.failedReads=recovered.failedReads;
    const auto failedRepeat=Diagnostics::AssessBalance(rows,50,25,false,false,failures);
    std::ostringstream failedReport;
    Diagnostics::PrintBalanceRipRecommendation(failedReport,failedRepeat);
    check(recovered.c2Total==0 && recovered.FailurePenalty()>0 &&
        failedRepeat.available && !failedRepeat.recommendationAvailable && failedRepeat.suggestedSpeed==0 &&
        failedReport.str().find("Failed read attempts: 1")!=std::string::npos &&
        Diagnostics::BalanceExtractionGuidance(failedRepeat).find("failed read attempts")!=std::string::npos,
        "26: successful retries preserve failure evidence without inventing C2 and withhold rip advice");
    failures.hardwareCuTotal=2;
    const auto both=Diagnostics::AssessBalance(rows,50,25,false,false,failures);
    check(Diagnostics::BalanceExtractionGuidance(both).find("Uncorrectable")!=std::string::npos,
        "26: uncorrectable evidence retains priority over recovered read failures");
    Diagnostics::BalanceReadEvidence c2Evidence;
    c2Evidence.readCdC2Total=repeats.c2Total;
    auto observedC2=Diagnostics::AssessBalance(rows,50,25,false,false,c2Evidence);
    std::ostringstream c2Report;
    Diagnostics::PrintBalanceRipRecommendation(c2Report,observedC2);
    check(observedC2.available && observedC2.score==full.score && !observedC2.recommendationAvailable &&
        observedC2.suggestedSpeed==0 &&
        Diagnostics::BalanceExtractionGuidance(observedC2).find("READ CD C2 observed")!=std::string::npos &&
        c2Report.str().find("successful reads: 18")!=std::string::npos,
        "26: positive repeat/re-test C2 survives a perfect mechanical score in extraction advice");
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
    auto speedRepeats=assess(rows);
    check(speedRepeats.available && speedRepeats.score==100 && speedRepeats.suggestedSpeed==16 && !speedRepeats.haveFullScore &&
        std::all_of(speedRepeats.timingPenalty.begin(),speedRepeats.timingPenalty.end(),[](int p){return p==0;}) &&
        std::all_of(speedRepeats.primaryCompared.begin(),speedRepeats.primaryCompared.end(),[](bool b){return b;}),
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
    check(readFailure.firstInsufficientReadSpeed==8 && readFailure.suggestedSpeed==4,
        "26: an excluded failed 8x measurement limits the recommendation to the measured 4x baseline");
    std::ostringstream coverageReport;
    Diagnostics::PrintBalanceRipRecommendation(coverageReport,readFailure);
    check(coverageReport.str().find("Insufficient readable samples at ~8x")!=std::string::npos,
        "26: the rip recommendation explains the excluded read-coverage limit");
    rows[1].validReads=24;rows[1].readTimeMs=20;
    check(assess(rows).suggestedSpeed==4, "26: insufficient partial coverage also blocks higher recommendations");
    rows[1].validReads=25;
    check(assess(rows).suggestedSpeed==40, "26: the minimum coverage boundary remains eligible");
    rows=clean();rows[0].validReads=0;rows[0].readTimeMs=0;
    const auto baselineFailure=assess(rows);
    check(baselineFailure.available && !baselineFailure.recommendationAvailable && baselineFailure.suggestedSpeed==0,
        "26: failure at the lowest measured speed leaves no recommendation despite faster usable rows");
    rows=clean();rows[1].actualSpeed=4;rows[1].validReads=0;
    check(!assess(rows).recommendationAvailable,
        "26: a failed repeat at the same actual speed cannot be hidden by another successful request");
    rows=clean();rows[0].actualSpeed=8;rows[0].validReads=0;rows[0].readTimeMs=0;
    rows[1].actualSpeed=4;rows[1].readTimeMs=40;
    check(assess(rows).suggestedActualSpeed==4,
        "26: the coverage limit follows actual speed rather than request order");
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
    for(size_t i=0;i<2;++i) {rows[i].hardwareActualSpeed=rows[i].actualSpeed;rows[i].hardwareVerified=true;}
    auto pioneer=Diagnostics::AssessBalance(rows,50,25,true,true);
    rows[0].secondStageRate=500;rows[1].secondStageRate=50000;
    auto pioneerE22=Diagnostics::AssessBalance(rows,50,25,true,true);
    check(pioneer.usingHwEcc && pioneerE22.score==pioneer.score && pioneerE22.suggestedSpeed==pioneer.suggestedSpeed,
        "26: extracting the scorer preserves Pioneer E22 as diagnostic-only");
    check(pioneerE22.firstC2WarningSpeed==0,
        "26: Pioneer E22 never activates the independent C2 recommendation limit");
    auto warningSweep=[&] {
        auto data=clean();
        const double c1[]={4.53,4.53,6.33,9.2,0,0};
        const double c2[]={0,0,5.0/15,53.0/15,0,0};
        for(size_t i=0;i<data.size();++i) {
            data[i].c1Rate=c1[i];data[i].secondStageRate=c2[i];data[i].hardwareSamples=15;
            data[i].hardwareActualSpeed=data[i].actualSpeed;data[i].hardwareVerified=i<4;
        }
        return data;
    };
    rows=warningSweep();
    auto verifiedBaseline=Diagnostics::AssessBalance(rows,50,25,true,false);
    rows[0].hardwareVerified=false;
    auto missingBaseline=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(verifiedBaseline.usingHwEcc && !missingBaseline.usingHwEcc &&
        verifiedBaseline.suggestedSpeed==16 && missingBaseline.suggestedSpeed==16 &&
        missingBaseline.firstC2WarningSpeed==24 && missingBaseline.recommendationAvailable,
        "26: removing baseline verification cannot lift a verified 24x C2 warning or raise the 16x recommendation");
    rows[0].hardwareSamples=0;rows[0].hardwareActualSpeed=0;
    check(Diagnostics::AssessBalance(rows,50,25,true,false).suggestedSpeed==16,
        "26: completely missing baseline counters still preserve other verified C2 limits");
    rows=warningSweep();rows[3].validReads=0;rows[3].readTimeMs=0;
    auto missingTiming=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(!missingTiming.compared[3] && missingTiming.firstC2WarningSpeed==24 && missingTiming.suggestedSpeed<=16,
        "26: independent hardware C2 evidence survives insufficient timing coverage at its speed");
    rows=warningSweep();rows[3].hardwareActualSpeed=8;
    auto otherPhaseSpeed=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(otherPhaseSpeed.firstC2WarningSpeed==8 && otherPhaseSpeed.suggestedSpeed==4,
        "26: the C2 limit follows the measured hardware speed rather than the requested or timing speed");
    rows=warningSweep();rows[0].secondStageRate=0.75;
    auto noLowerSpeed=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(noLowerSpeed.available && !noLowerSpeed.recommendationAvailable && noLowerSpeed.suggestedSpeed==0 &&
        noLowerSpeed.firstC2WarningSpeed==4,
        "26: a C2 warning at the lowest measured speed leaves the recommendation unestablished");
    rows[0].validReads=0;rows[0].readTimeMs=0;
    auto lowerFailure=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(lowerFailure.available && !lowerFailure.recommendationAvailable && lowerFailure.firstC2WarningSpeed==4,
        "26: excluding the lowest timing row cannot erase its separately verified hardware C2 warning");
    rows=warningSweep();rows[0].hardwareVerified=false;rows[3].hardwareVerified=false;
    auto unverifiedCounter=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(unverifiedCounter.firstC2WarningSpeed==0 && unverifiedCounter.recommendationAvailable,
        "26: unverified counters do not invent a verified speed limit");
    rows=warningSweep();rows[0].hardwareVerified=false;rows[3].secondStageRate=0.5;
    auto boundary=Diagnostics::AssessBalance(rows,50,25,true,false);
    rows[3].secondStageRate=std::nextafter(0.5,1.0);
    auto aboveBoundary=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(boundary.firstC2WarningSpeed==0 && aboveBoundary.firstC2WarningSpeed==24 && aboveBoundary.suggestedSpeed==16,
        "26: the existing C2 threshold remains strictly greater than 0.5 per measured second");
    rows=warningSweep();rows[3].secondStageRate=std::numeric_limits<double>::quiet_NaN();
    check(Diagnostics::AssessBalance(rows,50,25,true,false).firstC2WarningSpeed==0,
        "26: invalid rates cannot create an absolute C2 limit");
    auto clearedWarning=noLowerSpeed;
    clearedWarning=assess(clean());
    check(clearedWarning.firstC2WarningSpeed==0 && clearedWarning.recommendationAvailable && clearedWarning.suggestedSpeed==40,
        "26: a new measured sweep resets an old unavailable recommendation and old C2 limits");
    auto reused=limited;
    reused=assess(clean());
    check(reused.available && !reused.partial && reused.haveFullScore && reused.suggestedSpeed==40,
        "26: a new complete sweep clears previous limited-range state");
    rows=clean();for(auto& r:rows)r.actualSpeed=0;reused=assess(rows);
    check(!reused.available && reused.suggestedSpeed==0 && reused.score==0,
        "26: losing measurement coverage clears old scores and speed recommendations");
    return failed;
}

