#pragma once

enum Page { Typography, Position, Background, Scroll, Outline, Glow, Visual, PageCount };
enum class OptionKind { Number, Choice, Color };

struct Choice { const wchar_t* label; const char* value; };
inline constexpr Choice toggleChoices[]={{L"关闭","false"},{L"启用","true"},{nullptr,nullptr}};
inline constexpr Choice horizontalChoices[]={{L"左","left"},{L"中","center"},{L"右","right"},{nullptr,nullptr}};
inline constexpr Choice alignChoices[]={{L"左对齐","left"},{L"居中","center"},{L"右对齐","right"},{nullptr,nullptr}};
inline constexpr Choice verticalChoices[]={{L"上","top"},{L"中","center"},{L"下","bottom"},{nullptr,nullptr}};
inline constexpr Choice anchorHorizontalChoices[]={{L"跟随左侧","left"},{L"跟随中心","center"},{L"跟随右侧","right"},{nullptr,nullptr}};
inline constexpr Choice anchorVerticalChoices[]={{L"跟随顶部","top"},{L"跟随中心","center"},{L"跟随底部","bottom"},{nullptr,nullptr}};
inline constexpr Choice directionChoices[]={{L"向左","left"},{L"向右","right"},{nullptr,nullptr}};

struct Option {
    int id;
    Page page;
    const char* key;
    const wchar_t* label;
    OptionKind kind;
    double minimum=0,maximum=0;
    const Choice* choices=nullptr;
};

inline constexpr Option options[]={
    {200,Typography,"bold",L"粗体",OptionKind::Choice,0,0,toggleChoices},
    {201,Typography,"align",L"文字对齐",OptionKind::Choice,0,0,alignChoices},
    {202,Typography,"padding",L"文字边距",OptionKind::Number,0,100},
    {203,Typography,"letter_spacing",L"字距",OptionKind::Number,-10,50},
    {204,Typography,"color",L"文字颜色",OptionKind::Color},
    {205,Position,"x",L"X",OptionKind::Number,0,100000},
    {206,Position,"y",L"Y",OptionKind::Number,0,100000},
    {207,Position,"width",L"宽度",OptionKind::Number,8,100000},
    {208,Position,"height",L"高度",OptionKind::Number,8,100000},
    {209,Position,"anchor_x",L"横向定位",OptionKind::Choice,0,0,anchorHorizontalChoices},
    {210,Position,"anchor_y",L"纵向定位",OptionKind::Choice,0,0,anchorVerticalChoices},
    {211,Background,"background",L"背景颜色",OptionKind::Color},
    {212,Background,"opacity",L"不透明度",OptionKind::Number,0,255},
    {213,Scroll,"scroll",L"循环滚动",OptionKind::Choice,0,0,toggleChoices},
    {214,Scroll,"scroll_direction",L"方向",OptionKind::Choice,0,0,directionChoices},
    {215,Scroll,"scroll_speed",L"速度",OptionKind::Number,5,300},
    {216,Scroll,"scroll_gap",L"循环间距",OptionKind::Number,8,300},
    {217,Scroll,"scroll_feather",L"两侧渐隐",OptionKind::Choice,0,0,toggleChoices},
    {218,Scroll,"scroll_feather_width",L"羽化宽度",OptionKind::Number,1,200},
    {219,Outline,"outline",L"描边",OptionKind::Choice,0,0,toggleChoices},
    {220,Outline,"outline_width",L"描边宽度",OptionKind::Number,.5,12},
    {221,Outline,"outline_color",L"描边颜色",OptionKind::Color},
    {222,Glow,"glow",L"发光",OptionKind::Choice,0,0,toggleChoices},
    {223,Glow,"glow_color",L"发光颜色",OptionKind::Color},
    {224,Glow,"glow_radius",L"发光半径",OptionKind::Number,1,24},
    {225,Glow,"glow_strength",L"强度 (%)",OptionKind::Number,0,100},
    {226,Glow,"glow_pulse",L"呼吸动画",OptionKind::Choice,0,0,toggleChoices},
    {227,Glow,"glow_period",L"呼吸周期 (秒)",OptionKind::Number,.5,10},
};

inline constexpr const wchar_t* pageLabels[]={L"文字",L"位置",L"背景",L"滚动",L"描边",L"发光",L"视觉定位"};
inline constexpr int pageButton=300,optionLabel=1000,optionPicker=2000;
