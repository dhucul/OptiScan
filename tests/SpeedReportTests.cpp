#define NOMINMAX
#include "../HardwareSweep.h"
#include <iostream>
#include <sstream>

int RunSpeedReportTests() {
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n';if(!ok)++failures;
    };
    auto capture=[](int totalC1,int totalSecond,int speed,bool verified=true) {
        Diagnostics::HardwareSweepPass pass;
        pass.attempted=pass.complete=pass.cacheCleared=true;
        pass.speed.Record(speed>0,static_cast<WORD>(speed*CD_SPEED_1X));
        pass.speed.Record(speed>0,static_cast<WORD>(speed*CD_SPEED_1X));
        for(unsigned i=0;i<15;++i)
            pass.intervals.push_back({1000+i*75,75,totalC1/15+(i<static_cast<unsigned>(totalC1%15)?1:0)});
        pass.c1=ScanQuality::SummarizeC1(pass.intervals,verified && speed>0);
        pass.secondStageTotal=totalSecond;
        pass.limitation=verified && speed>0 ? "" : "counter or speed evidence unverified";
        return pass;
    };
    const auto first=capture(61,0,10);
    const auto second=capture(76,15,10);
    std::ostringstream report;
    Diagnostics::PrintHardwareSweepGroups(report,{{first,4,10},{second,8,10}},"C2");
    auto text=report.str();
    check(text.find("No C2 reported in this target; errors outside this window are not measured.")!=std::string::npos,
        "A sampled hardware zero cannot be read as a whole-disc C2 total");
    check(text.find("~10x hardware observations - repeated measurements (2 passes)")!=std::string::npos &&
        text.find("4x hardware observations")==std::string::npos && text.find("8x hardware observations")==std::string::npos,
        "Clamped 4x/8x requests appear as repeat measurements at reported 10x");
    check(text.find("C1 by pass: 4.07/sec [EXCELLENT]; 5.07/sec [GOOD]")!=std::string::npos &&
        text.find("EXCELLENT is below 5.00/sec; GOOD is 5.00 to below 50.00/sec")!=std::string::npos &&
        text.find("show that the disc deteriorated or that a higher requested speed made it worse")!=std::string::npos,
        "The user's EXCELLENT/GOOD example explains its numeric cutoff without diagnosing deterioration");
    check(text.find("C2 raw totals by pass: 0; 15")!=std::string::npos &&
        text.find("does not cancel the positive observation")!=std::string::npos &&
        text.find("average 1.00/sec")!=std::string::npos && text.find("average 0.50/sec")==std::string::npos,
        "Zero and positive C2 observations remain separate instead of being averaged into a quieter result");
    check(text.find("Requested: 4x")<text.find("Requested: 8x") &&
        text.find("Total C1 observed: 61")!=std::string::npos && text.find("Total C1 observed: 76")!=std::string::npos,
        "Requested settings, acquisition order and raw counts survive grouping");
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{second,8,10},{first,4,10}},"C2");
    check(report.str().find("Requested: 8x")<report.str().find("Requested: 4x") &&
        report.str().find("C2 raw totals by pass: 15; 0")!=std::string::npos,
        "Reverse acquisition order is retained within an actual-speed group");
    const auto groups=Diagnostics::GroupMeasuredSpeeds({16,10,0,10,-1});
    check(groups.size()==3 && groups[0].speed==10 && groups[0].rows==std::vector<size_t>({1,3}) &&
        groups[1].speed==16 && groups[1].rows==std::vector<size_t>({0}) &&
        groups[2].speed==0 && groups[2].rows==std::vector<size_t>({2,4}),
        "Grouping preserves source row identity and separates unknown speeds");
    const auto unknown=capture(30,9,0,false);
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{unknown,24,24},{unknown,32,32}},"C2");
    text=report.str();
    check(text.find("actual speed UNVERIFIED (2 passes)")!=std::string::npos &&
        text.find("repeated measurements")==std::string::npos && text.find("~0x")==std::string::npos &&
        text.find("NOT RATED")!=std::string::npos && text.find("C2 observed total: 9")!=std::string::npos,
        "Unknown speed captures are not asserted to be repeats at a shared physical speed");
    const auto unrated=capture(0,0,10,false);
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{first,4,10},{unrated,8,10}},"C2");
    text=report.str();
    check(text.find("C1 by pass: 4.07/sec [EXCELLENT]; NOT RATED")!=std::string::npos &&
        text.find("C2 raw totals by pass: 0; 0 (unrated)")!=std::string::npos &&
        text.find("0.00/sec [EXCELLENT]")==std::string::npos && text.find("C1 label explanation")==std::string::npos,
        "Unrated captures retain their limitations without creating a clean average or a false band change");
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{first,4,20}},"C2");
    check(report.str().find("~10x hardware observations")!=std::string::npos &&
        report.str().find("~20x hardware observations")==std::string::npos &&
        report.str().find("different/unverified speeds")!=std::string::npos,
        "Hardware grouping uses the hardware pass readback rather than the earlier timing sweep");
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{first,4,10},{second,8,10}},"E22");
    check(report.str().find("E22 raw totals by pass: 0; 15")!=std::string::npos &&
        report.str().find("C2 raw totals")==std::string::npos,
        "Grouping preserves Pioneer E22 counter identity");
    const Diagnostics::HardwareSweepPass notRun;
    report.str("");report.clear();
    Diagnostics::PrintHardwareSweepGroups(report,{{notRun,32,32},{first,4,10},{second,8,10}},"C2");
    text=report.str();
    check(text.find("repeated measurements (2 passes)")!=std::string::npos &&
        text.find("Requested 32x: NOT MEASURED")!=std::string::npos &&
        text.find("C2 raw totals by pass: 0; 15")!=std::string::npos,
        "Unrun settings are excluded from repeat counts while source indices remain correctly paired");
    report.str("");report.clear();report<<std::hex<<std::scientific<<std::setprecision(7);
    const auto flags=report.flags();const auto precision=report.precision();
    Diagnostics::PrintHardwareSweepGroups(report,{{first,4,10},{second,8,10}},"C2");
    check(report.flags()==flags && report.precision()==precision &&
        report.str().find("C2 raw totals by pass: 0; 15")!=std::string::npos,
        "Grouped reports render decimal observations and restore caller stream formatting");
    return failures;
}
