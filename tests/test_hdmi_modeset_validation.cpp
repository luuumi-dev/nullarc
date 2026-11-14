#include "phx_dcn314_modeset.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

struct Fake { unsigned writes=0, delays=0; };
static uint32_t readReg(void *, uint32_t) { return 0; }
static void writeReg(void *p, uint32_t, uint32_t) { static_cast<Fake *>(p)->writes++; }
static uint32_t readVram(void *, uint64_t) { return 0; }
static void writeVram(void *p, uint64_t, uint32_t) { static_cast<Fake *>(p)->writes++; }
static void delay(void *p, unsigned) { static_cast<Fake *>(p)->delays++; }
int main() {
    Fake fake;
    phxdcn314::Io io{readReg,writeReg,delay,&fake};
    phxdmubinbox::Io dio{}; phxdmubinbox::Inbox inbox{};
    phxdcn314::DpmsCtx ctx(io,dio,&inbox,phxdmubcmd::Signal::Dvi,148500);
    phxtiming::Mode mode{148500,1920,88,44,2200,1080,4,5,1125,true,true};
    const PhxDmlVal empty[]={{nullptr,0}};
    // An incomplete DML plan must leave the live display untouched.
    auto r=phxdcn314::modeset(ctx,mode,empty,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0 && fake.delays==0);
    r=phxdcn314::modeset(ctx,mode,nullptr,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0);
    // A valid mode still needs a complete, located firmware command channel.
    r=phxdcn314::modeset(ctx,mode,kDml60,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0 && fake.delays==0);
    assert(ctx.dmub==phxdmubinbox::Status::NotReady);
    ctx.dio={readReg,writeReg,readVram,writeVram,delay,&fake};
    ctx.ib=nullptr;
    r=phxdcn314::modeset(ctx,mode,kDml60,phxdmubcmd::Signal::Hdmi);
    assert(fake.writes==0 && fake.delays==0);
    ctx.ib=&inbox;
    for (unsigned capacity : {0u,64u,129u,128u}) {
        inbox.capacity=capacity;
        r=phxdcn314::modeset(ctx,mode,kDml60,phxdmubcmd::Signal::Hdmi);
        assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0 && fake.delays==0);
    }
    auto bad=mode; bad.hTotal=0;
    r=phxdcn314::modeset(ctx,bad,kDml60,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0);
    bad=mode; bad.vFrontPorch=UINT32_MAX;
    r=phxdcn314::modeset(ctx,bad,kDml60,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0);
    bad=mode; bad.hTotal=32769;
    r=phxdcn314::modeset(ctx,bad,kDml60,phxdmubcmd::Signal::Hdmi);
    assert(r.frontEnd==phxdcn314::Result::BadPlan && fake.writes==0);
}
