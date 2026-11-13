#pragma once
#include <algorithm>
#include <cmath>

// Scheduling and presentation are independent. A failed check never changes
// the last confirmed client-relative rectangle.
struct VisualRefreshSchedule {
    bool requested=true,active=false;
    double readyAt=0,nextAt=0,deadline=0;
    void request(double now,double settle=.15){requested=true;active=false;readyAt=now+settle;}
    int wait_ms(double now)const{
        if(active||requested&&now>=readyAt)return 50;
        return requested?std::clamp(int(std::ceil((readyAt-now)*1000)),1,1000):1000;
    }
    bool due(double now,int interval)const{
        return active||(requested?now>=readyAt:interval>0&&now>=nextAt);
    }
    void start(double now){active=true;requested=false;deadline=now+8;}
    void finish(double now,int interval){active=requested=false;nextAt=interval>0?now+interval:0;}
};
