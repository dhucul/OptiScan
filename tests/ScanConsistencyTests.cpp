#define NOMINMAX
#include "../HardwareSweep.h"
#include "../DiscRotQuality.h"
#include "../DiscBalanceAssessment.h"
#include <cmath>
#include <iostream>
#include <sstream>

int RunScanConsistencyTests() {
    int failed=0;
    auto check=[&](bool ok,const char* label) {
        std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n';if(!ok)++failed;
    };
    for (const auto [elapsed,expected]: {std::pair{1000,4}, {400,10}, {250,16}}) {
        Diagnostics::ScanThroughput speed;
        speed.Begin(10000);speed.Observe(300,10000+elapsed);speed.Finish(10000+elapsed);
        check(std::abs(speed.AverageX()-expected)<1e-8 &&
            Diagnostics::ScanSpeedText(speed.AverageX())==std::to_string(expected)+"x",
            "Measured audio coverage over wall time displays 4x, 10x and 16x without using a drive speed request");
    }
    Diagnostics::ScanThroughput full;
    full.Begin(0);full.Observe(316267,1662000);full.Finish(1662000);
    Diagnostics::HardwareSpeedEvidence reported;
    reported.Record(true,10*CD_SPEED_1X);reported.Record(true,10*CD_SPEED_1X);
    std::ostringstream report;
    Diagnostics::PrintScanTelemetry(report,reported,full);
    check(full.AverageX()>2.537 && full.AverageX()<2.538 &&
        report.str().find("~10x")!=std::string::npos && report.str().find("average): 2.5x")!=std::string::npos,
        "The user's 70-minute audio scan taking 27m42s measures 2.5x throughput independently of its 10x readback");
    Diagnostics::ScanThroughput fast;
    fast.Begin(0);fast.Observe(1125,1500);fast.Finish(1500);
    std::vector<ScanQuality::C1Interval> intervals;
    for (unsigned i=0;i<15;++i) intervals.push_back({i*75,75,1});
    const auto counts=ScanQuality::SummarizeC1(intervals,true);
    check(fast.AverageX()==10.0 && counts.average==1.0 && counts.MeasuredSeconds()==15.0,
        "A 10x scan still normalizes correction counts per audio second, never per faster wall-clock second");
    auto frozen=full.AverageX();full.Finish(2000000);full.Observe(75,2001000);
    check(full.AverageX()==frozen,"Cleanup or a later pass cannot change a completed throughput measurement");
    full.Begin(0);full.Observe(150,1000);full.Observe(0,2000);
    check(full.AverageX()==0 && full.CurrentX()==0,
        "Unknown sample coverage cannot fabricate a scan speed from poll count");
    full.Begin(1000);full.Observe(75,999);
    check(full.AverageX()==0,"Regressing timestamps cannot underflow into a false measured speed");
    full.Begin(0);full.Observe(300,1000);full.Observe(300,2000);
    full.Observe(75,3000);full.Observe(75,4000);
    check(std::abs(full.CurrentX()-1.0)<1e-8 && std::abs(full.AverageX()-2.5)<1e-8,
        "Live throughput follows the recent two-second slowdown while the overall average remains separate");

    using Decision=Diagnostics::QualitySampleDecision;
    Diagnostics::QualitySampleSequence sequence(1000,2124);
    check(sequence.Observe(1000,0,9,15,2,true)==Decision::Accept,
        "All quality workflows retain first-sample C2/CU even when duration is unknown");
    check(sequence.Observe(1000,0,9,15,2,true)==Decision::Ignore,
        "Duplicate terminal positions cannot count the same C2/CU observation twice");
    check(sequence.Observe(1075,75,4,0,0,true)==Decision::Accept &&
        sequence.Observe(1050,75,4,0,0,true)==Decision::Invalid,
        "Regressing or overlapping interval positions fail consistently across scan workflows");
    check(sequence.Observe(2200,75,4,0,0,false)==Decision::Ignore &&
        sequence.Observe(2100,75,4,0,0,true)==Decision::Invalid,
        "Backend completion markers are ignored and measured intervals must stay inside the requested window");

    QCheckResult quality;quality.scanMethod="LiteOn/MediaTek (measured intervals)";
    quality.samples.push_back({1000,9,15,2,0,0});
    quality.totalC1=9;quality.totalC2=15;quality.totalCU=2;ComputeTimedC1(quality);
    DiscRotAnalysis rot;DiscRot::RecordQualityEvidence(quality,true,rot);
    check(rot.qualityC2Count==15 && rot.qualityCUCount==2 && !quality.c1.RateAvailable(),
        "Disc Rot retains the same raw Q-Check C2/CU evidence without assigning unknown-duration C1 a rate");

    std::vector<Diagnostics::BalanceSpeedSample> rows;
    const int requests[]={4,8,16,24,32,40}, actual[]={10,10,16,24,32,40};
    for(int i=0;i<6;++i) {
        Diagnostics::BalanceSpeedSample row;
        row.requestedSpeed=requests[i];row.actualSpeed=actual[i];row.validReads=50;
        row.readTimeMs=700.0/actual[i];row.jitterCV=0.1;row.stabilityRatio=1.03;row.stabilityMeasured=true;
        row.hardwareSamples=15;row.hardwareVerified=true;row.hardwareActualSpeed=actual[i];
        row.c1Rate=i<2?3:9;row.secondStageRate=i<2?0:3.2;rows.push_back(row);
    }
    const auto assessment=Diagnostics::AssessBalance(rows,50,25,true,false);
    report.str("");report.clear();Diagnostics::PrintBalanceRipRecommendation(report,assessment);
    check(assessment.suggestedSpeed==8 && assessment.suggestedActualSpeed==10 &&
        report.str().find("request 8x (drive-reported speed ~10x)")!=std::string::npos,
        "A clamped 8x recommendation explicitly states the drive actually reported 10x");
    check(Diagnostics::BalanceExtractionGuidance(assessment).find("Caution - C2 warning at ~16x")!=std::string::npos &&
        report.str().find("C2 warning at ~16x")!=std::string::npos,
        "A good mechanical score cannot produce reassuring extraction advice at a speed with a C2 warning");

    report.str("");report.clear();report<<std::hex<<std::scientific<<std::setprecision(6);
    auto flags=report.flags();auto precision=report.precision();
    Diagnostics::PrintScanTelemetry(report,reported,full);
    check(flags==report.flags() && precision==report.precision(),
        "Speed reporting restores caller formatting for the remaining report");
    int stops=0,closes=0;
    {
        QualityScanSession broken([&]{++stops;return false;});
        check(!broken.StopOrClose([&]{++closes;}) && !broken.StopOrClose([&]{++closes;}),
            "A failed quality-session stop transitions to a terminal closed state");
    }
    check(stops==2 && closes==1,
        "A closed session never reissues vendor stop commands from its destructor");
    auto cuRows=rows;
    for (auto& row:cuRows) row.secondStageRate=0;
    const auto mechanical=Diagnostics::AssessBalance(cuRows,50,25,true,false);
    const auto cuWarning=Diagnostics::AssessBalance(cuRows,50,25,true,false,{7,0});
    report.str("");report.clear();Diagnostics::PrintBalanceRipRecommendation(report,cuWarning);
    check(cuWarning.score==mechanical.score && !cuWarning.recommendationAvailable &&
        cuWarning.suggestedSpeed==0 && cuWarning.suggestedActualSpeed==0 &&
        Diagnostics::BalanceExtractionGuidance(cuWarning).find("Uncorrectable")!=std::string::npos &&
        report.str().find("Hardware CU observed: 7")!=std::string::npos &&
        report.str().find("request 8x")==std::string::npos,
        "Positive CU overrides rip advice without rewriting the mechanical score");
    for (auto& row:cuRows) row.hardwareVerified=false;
    const auto partialCu=Diagnostics::AssessBalance(cuRows,50,25,true,false,{7,0});
    check(!partialCu.recommendationAvailable && partialCu.readEvidence.hardwareCuTotal==7,
        "Unrated hardware passes cannot erase observed uncorrectable activity from extraction advice");
    const auto pioneerLoss=Diagnostics::AssessBalance(cuRows,50,25,true,true,{0,12});
    report.str("");report.clear();Diagnostics::PrintBalanceRipRecommendation(report,pioneerLoss);
    check(!pioneerLoss.recommendationAvailable && pioneerLoss.firstC2WarningSpeed==0 &&
        report.str().find("Pioneer CD Check uncorrectable bytes: 12")!=std::string::npos,
        "Pioneer data-loss evidence overrides advice independently of its diagnostic E22 counters");
    const auto cleanAgain=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(!cleanAgain.readEvidence.HasUncorrectable() && cleanAgain.recommendationAvailable,
        "A new Balance assessment does not retain an earlier disc's uncorrectable flag");
    return failed;
}
