#pragma once

#include <string>
#include <vector>

// PC 端映射配置。普通按键/视角/后续轮盘都放在统一 profile 中。
enum class PcMappingTriggerType : int {
    KeyboardVk = 1,
    MouseButton = 2,     // 1左键/2右键/3中键/4侧键1/5侧键2
};

enum class PcMappingActionType : int {
    None = 0,
    TouchTap = 1,
    TouchHold = 2,
    SendLinuxKey = 3,
};

// 普通按键按住效果。
enum class PcKeyTouchMode : int {
    RandomMove = 1,      // 按下和按住移动都限制在按钮圆形半径内
    RandomDownUp = 2,    // 按下固定中心，不随机移动
};

// 普通按键特殊动作。对应旧悬浮窗里的“无 / 上车 / 下车”。
enum class PcKeySpecialAction : int {
    None = 0,
    ReclickLockOnKeyDown = 1,
    DismountReclickLockOnKeyDown = 2,
};

enum class PcLockSlideTouchMode : int {
    SingleReanchor = 1,
    DualSimultaneous = 2,
    DualSequential = 3,
};

// 轮盘运动模式。FixedCenter = 固定中心轮盘；MouseSway = 按键方向基础上跟随鼠标轻微偏航。
enum class PcCompassMotionMode : int {
    FixedCenter = 1,
    MouseSway = 2,
};
// 轮盘默认运动模式。
// 以后要改默认固定/晃动，只改这里。
static constexpr PcCompassMotionMode PC_COMPASS_MOTION_MODE_DEFAULT =
PcCompassMotionMode::MouseSway;
// ==============================
// 轮盘参数统一范围 / 默认值
// ==============================
// 说明：
// - UI 滑条显示的半径单位是“千分归一化单位”：80 表示 80000。
// - 运行时保存字段仍使用 Norm：0~1000000。
// - 后续如果要改轮盘设置范围，只改这里，不要在弹窗/运行时/创建逻辑里写裸数字。
static constexpr int PC_COMPASS_RADIUS_UNIT = 1000;

// 轮盘半径，UI 显示单位。
// 内半径：默认 80，允许 60~140。
static constexpr int PC_COMPASS_INNER_RADIUS_MIN = 80;
static constexpr int PC_COMPASS_INNER_RADIUS_MAX = 150;
static constexpr int PC_COMPASS_INNER_RADIUS_DEFAULT = 100;

// 外半径：默认 190，允许 70~200。 外半径：默认 190，允许 90~250,外半径必须至少比内半径大 PC_COMPASS_RADIUS_MIN_GAP。
static constexpr int PC_COMPASS_OUTER_RADIUS_MIN = 90;
static constexpr int PC_COMPASS_OUTER_RADIUS_MAX = 250;
static constexpr int PC_COMPASS_OUTER_RADIUS_DEFAULT = 230;
static constexpr int PC_COMPASS_RADIUS_MIN_GAP = 30;

static constexpr int PC_COMPASS_INNER_RADIUS_MIN_NORM = PC_COMPASS_INNER_RADIUS_MIN * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_INNER_RADIUS_MAX_NORM = PC_COMPASS_INNER_RADIUS_MAX * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_INNER_RADIUS_DEFAULT_NORM = PC_COMPASS_INNER_RADIUS_DEFAULT * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_OUTER_RADIUS_MIN_NORM = PC_COMPASS_OUTER_RADIUS_MIN * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_OUTER_RADIUS_MAX_NORM = PC_COMPASS_OUTER_RADIUS_MAX * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_OUTER_RADIUS_DEFAULT_NORM = PC_COMPASS_OUTER_RADIUS_DEFAULT * PC_COMPASS_RADIUS_UNIT;
static constexpr int PC_COMPASS_RADIUS_MIN_GAP_NORM = PC_COMPASS_RADIUS_MIN_GAP * PC_COMPASS_RADIUS_UNIT;

// X/Y 速度滑条。10 为 1.0 倍基准速度。
static constexpr int PC_COMPASS_SPEED_STEP_MIN = 15;
static constexpr int PC_COMPASS_SPEED_STEP_MAX =30;
static constexpr int PC_COMPASS_SPEED_STEP_DEFAULT = 15;

// 晃动模式扇区范围。
// 100 = 当前方向完整 1/16 扇区，即方向中心 ±22.5°。
// 正向用于 W/A/S/D；斜向用于 LU/RU/LD/RD。
static constexpr int PC_COMPASS_SWAY_SECTOR_MIN = 25;
static constexpr int PC_COMPASS_SWAY_SECTOR_MAX = 110;
static constexpr int PC_COMPASS_SWAY_SECTOR_DEFAULT = 60;

//斜向扇区范围
static constexpr int PC_COMPASS_SWAY_DIAG_SECTOR_MIN = 15;
static constexpr int PC_COMPASS_SWAY_DIAG_SECTOR_MAX = 60;
static constexpr int PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT = 50;

// 晃动模式每段目标点范围。
// min/max 是当前方向扇区半宽的百分比。
static constexpr int PC_COMPASS_SWAY_STEP_MIN_MIN = 5;
static constexpr int PC_COMPASS_SWAY_STEP_MIN_MAX = 95;
static constexpr int PC_COMPASS_SWAY_STEP_MIN_DEFAULT = 33;
static constexpr int PC_COMPASS_SWAY_STEP_MAX_MIN = 6;
static constexpr int PC_COMPASS_SWAY_STEP_MAX_MAX = 100;
static constexpr int PC_COMPASS_SWAY_STEP_MAX_DEFAULT = 66;
static constexpr int PC_COMPASS_SWAY_STEP_MIN_GAP = 1;

// 晃动模式扇区内部移动速度，只影响同一方向内游走，不影响切换方向。
static constexpr int PC_COMPASS_SWAY_SPEED_MIN = 1;
static constexpr int PC_COMPASS_SWAY_SPEED_MAX = 10;
static constexpr int PC_COMPASS_SWAY_SPEED_DEFAULT = 2;

// 固定模式的轻量平滑游走参数。
// 不使用鼠标稳定逻辑；范围更小，默认只在当前方向 30% 扇区内慢速滑动。
static constexpr int PC_COMPASS_FIXED_SOFT_SWAY_SECTOR_PERCENT = 30;
static constexpr int PC_COMPASS_FIXED_SOFT_SWAY_DIAG_SECTOR_PERCENT = 30;
static constexpr int PC_COMPASS_FIXED_SOFT_SWAY_SPEED_PERCENT = 1;

// 鼠标稳定模式：点击鼠标左/右键后，在持续时间内使用更小扇区。
static constexpr int PC_COMPASS_MOUSE_STABLE_SECTOR_MIN = 5;
static constexpr int PC_COMPASS_MOUSE_STABLE_SECTOR_MAX = 80;
static constexpr int PC_COMPASS_MOUSE_STABLE_SECTOR_DEFAULT = 33;
static constexpr int PC_COMPASS_MOUSE_STABLE_SPEED_MIN = 5;
static constexpr int PC_COMPASS_MOUSE_STABLE_SPEED_MAX = 50;
static constexpr int PC_COMPASS_MOUSE_STABLE_SPEED_DEFAULT = 25;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MIN = 1;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MAX = 8;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_SEC_DEFAULT = 4;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_MS_MIN = PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MIN * 1000;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_MS_MAX = PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MAX * 1000;
static constexpr int PC_COMPASS_MOUSE_STABLE_HOLD_MS_DEFAULT = PC_COMPASS_MOUSE_STABLE_HOLD_SEC_DEFAULT * 1000;

constexpr int PcClampCompassInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}


struct PcCompassButtonBinding {
    std::vector<int> comboTriggerCodes;
    std::wstring triggerLabel;
};

struct PcCompassBinding {
    std::string id;
    int centerXNorm = 240000;
    int centerYNorm = 760000;
    int innerRadiusNorm = PC_COMPASS_INNER_RADIUS_DEFAULT_NORM;
    int outerRadiusNorm = PC_COMPASS_OUTER_RADIUS_DEFAULT_NORM;
    int speedXStep = PC_COMPASS_SPEED_STEP_DEFAULT;
    int speedYStep = PC_COMPASS_SPEED_STEP_DEFAULT;
    // 旧字段保留兼容读取。当前晃动手感不再通过触摸Hz/强度控制。
    int touchSampleHz = 240;
    int swaySensitivity = 55;

    // 正向扇区范围：100 = W/A/S/D 每个方向完整 1/16 扇区，即方向中心 ±22.5°；
    // 小于 100 收窄，大于 100 允许少量跨出当前方向扇区。
    int swaySectorPercent = PC_COMPASS_SWAY_SECTOR_DEFAULT;
    // 斜向扇区范围默认值：50 = LU/RU/LD/RD 每个方向半个 1/16 扇区；最高限制为 90。
    // 新版允许 4 个斜向方向独立调整；这个字段作为设置面板的全局默认/兼容旧配置。
    int swayDiagonalSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;
    int swayDiagLeftUpSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;
    int swayDiagRightUpSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;
    int swayDiagLeftDownSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;
    int swayDiagRightDownSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;

    // 晃动模式参数：在当前方向扇区内选取下一个目标点。
    // 最小/最大扇区：目标点相对当前方向中心线的角度范围，占当前扇区半宽的百分比。
    int swayStepMinPercent = PC_COMPASS_SWAY_STEP_MIN_DEFAULT;
    int swayStepMaxPercent = PC_COMPASS_SWAY_STEP_MAX_DEFAULT;
    // 扇区移动速度百分比。默认 5，范围 2~20。只影响扇区内部游走，不影响切方向。
    int swaySpeedPercent = PC_COMPASS_SWAY_SPEED_DEFAULT;
    // 鼠标左键或右键触发稳定模式：检测到点击后，接下来数秒使用小范围扇区移动。
    bool swayMouseButtonSmallStep = false;
    // 鼠标稳定模式最大扇区：默认当前扇区半宽的 1/3。
    int swayMouseButtonStepPercent = PC_COMPASS_MOUSE_STABLE_SECTOR_DEFAULT;
    // 鼠标稳定模式扇区移动速度。默认 25，最高 50。
    int swayMouseButtonUpdatePercent = PC_COMPASS_MOUSE_STABLE_SPEED_DEFAULT;
    // 鼠标稳定模式持续时间，默认 4 秒。
    int swayMouseButtonHoldMs = PC_COMPASS_MOUSE_STABLE_HOLD_MS_DEFAULT;
    int slot = 1;
    PcCompassMotionMode motionMode = PC_COMPASS_MOTION_MODE_DEFAULT;//摇杆WASD默认晃动模式
    bool centerOuterReversed = false;

    PcCompassButtonBinding up;
    PcCompassButtonBinding left;
    PcCompassButtonBinding down;
    PcCompassButtonBinding right;
    PcCompassButtonBinding center;

    // 预留给“晃动”模式的四个对角锚点；第一版先按旧结构保存和绘制，运行时先使用方向偏航算法。
    int diagLeftUpXNorm = -1;
    int diagLeftUpYNorm = -1;
    int diagRightUpXNorm = -1;
    int diagRightUpYNorm = -1;
    int diagLeftDownXNorm = -1;
    int diagLeftDownYNorm = -1;
    int diagRightDownXNorm = -1;
    int diagRightDownYNorm = -1;
};


// 菜单映射分类。PC 端负责所有菜单状态/坐标计算，Android 只执行最终 slot 触摸。
enum class PcMenuCategory : int {
    NormalKey = 1,
    MenuRadial = 2,
    MenuItemOperation = 3,
    MenuBagOperation = 4,
    MenuHorizontal = 5,
    MenuVertical = 6,
};

enum class PcMenuShapeType : int {
    Radial = 1,
    Horizontal = 2,
    Vertical = 3,
    ItemOperation = 4,
    BagOperation = 5,
};

enum class PcMenuTriggerPlacement : int {
    Left = 1,
    Right = 2,
    Top = 3,
    Bottom = 4,
};

enum class PcItemLandingZone : int {
    Left = 1,
    Right = 2,
    Top = 3,
    Bottom = 4,
};


// 通用圆形按钮 UI 半径，UI 显示单位。
// 保存字段仍使用 Norm：20 表示 20000，60 表示 60000，默认 30。
// 普通按键、轮盘菜单、道具/背包三个圆点共用这组范围；视角相关半径仍走自己的参数。
static constexpr int PC_BUTTON_RADIUS_UNIT = 1000;
static constexpr int PC_BUTTON_RADIUS_MIN = 20;
static constexpr int PC_BUTTON_RADIUS_MAX = 60;
static constexpr int PC_BUTTON_RADIUS_DEFAULT = 30;
static constexpr int PC_BUTTON_RADIUS_MIN_NORM = PC_BUTTON_RADIUS_MIN * PC_BUTTON_RADIUS_UNIT;
static constexpr int PC_BUTTON_RADIUS_MAX_NORM = PC_BUTTON_RADIUS_MAX * PC_BUTTON_RADIUS_UNIT;
static constexpr int PC_BUTTON_RADIUS_DEFAULT_NORM = PC_BUTTON_RADIUS_DEFAULT * PC_BUTTON_RADIUS_UNIT;

// 兼容旧代码命名：菜单按钮半径实际也使用通用按钮半径范围。
static constexpr int PC_MENU_BUTTON_RADIUS_UNIT = PC_BUTTON_RADIUS_UNIT;
static constexpr int PC_MENU_BUTTON_RADIUS_MIN = PC_BUTTON_RADIUS_MIN;
static constexpr int PC_MENU_BUTTON_RADIUS_MAX = PC_BUTTON_RADIUS_MAX;
static constexpr int PC_MENU_BUTTON_RADIUS_DEFAULT = PC_BUTTON_RADIUS_DEFAULT;
static constexpr int PC_MENU_BUTTON_RADIUS_MIN_NORM = PC_BUTTON_RADIUS_MIN_NORM;
static constexpr int PC_MENU_BUTTON_RADIUS_MAX_NORM = PC_BUTTON_RADIUS_MAX_NORM;
static constexpr int PC_MENU_BUTTON_RADIUS_DEFAULT_NORM = PC_BUTTON_RADIUS_DEFAULT_NORM;

static constexpr int PC_MENU_ITEM_WHEEL_DISTANCE_MIN = 40;
static constexpr int PC_MENU_ITEM_WHEEL_DISTANCE_MAX = 800;
static constexpr int PC_MENU_ITEM_WHEEL_DISTANCE_DEFAULT = 200;
static constexpr int PC_MENU_ITEM_WHEEL_STEP_MIN = 5;
static constexpr int PC_MENU_ITEM_WHEEL_STEP_MAX = 120;
static constexpr int PC_MENU_ITEM_WHEEL_STEP_DEFAULT = 20;
static constexpr int PC_MENU_ITEM_WHEEL_SPEED_MIN = 1;
static constexpr int PC_MENU_ITEM_WHEEL_SPEED_MAX = 10;
static constexpr int PC_MENU_ITEM_WHEEL_SPEED_DEFAULT = 5;

enum class PcMenuTriggerMode : int {
    HoldRelative = 1,      // 按住触发，鼠标相对移动控制菜单
    HoldFreeCursor = 2,    // 按住触发，进入自由鼠标/道具操作
};

struct PcMenuBinding {
    std::string id;
    bool enabled = true;
    PcMenuCategory category = PcMenuCategory::MenuRadial;
    PcMenuShapeType shapeType = PcMenuShapeType::Radial;
    PcMenuTriggerMode triggerMode = PcMenuTriggerMode::HoldRelative;
    PcMenuTriggerPlacement triggerPlacement = PcMenuTriggerPlacement::Bottom;
    PcItemLandingZone itemLandingZone = PcItemLandingZone::Bottom;

    std::vector<int> comboTriggerCodes;
    std::wstring triggerLabel;

    int centerXNorm = 500000;
    int centerYNorm = 500000;
    int radiusNorm = PC_MENU_BUTTON_RADIUS_DEFAULT_NORM;
    int slot = 10;
    int segmentCount = 6;
    bool visualHintEnabled = true;

    // 菜单触点活动范围。-1 表示旧配置/未显式设置：
    // - 轮盘菜单按 center ± radius 自动生成活动范围；
    // - 道具/背包自由鼠标默认使用全屏范围。
    int rangeLeftNorm = -1;
    int rangeTopNorm = -1;
    int rangeRightNorm = -1;
    int rangeBottomNorm = -1;

    // 道具/背包触发键圆点位置。触发时会先在该圆形 UI 范围内随机按下/抬起一次。
    int triggerXNorm = 500000;
    int triggerYNorm = 500000;

    // 道具/背包二阶段自由鼠标落点（触发键按下/按住后，释放视角 slot 并把自由鼠标放到这里）。
    int freeCursorXNorm = 500000;
    int freeCursorYNorm = 500000;

    // 道具/背包关闭点。右键按下时先在该圆形范围内随机点击一次，再恢复视角锁定。
    int closeXNorm = 500000;
    int closeYNorm = 500000;

    int freeCursorSpeedNorm = 1600; // 旧配置兼容字段：HLMAP13 起不再作为运行时速度。

    // 菜单运行时自己的鼠标相对移动速度。单位是客户端像素倍率，支持小数累积。
    // 默认 0.5：RawInput dx=1 时，触点移动 0.5 个画面像素；取值范围 0.4~1.2。
    double relativeSpeedX = 0.5;
    double relativeSpeedY = 0.5;

    // 道具/背包操作参数。当前先由 PC 设置面板保存，后续运行时滚轮/落点行为直接读取这里。
    bool itemWheelScrollEnabled = false;
    bool itemWheelInvert = false;
    int itemWheelScrollSpeed = PC_MENU_ITEM_WHEEL_SPEED_DEFAULT;
    int itemWheelMaxDistancePx = PC_MENU_ITEM_WHEEL_DISTANCE_DEFAULT;
    int itemWheelStepPx = PC_MENU_ITEM_WHEEL_STEP_DEFAULT;
};

struct PcLockBinding {
    std::string id;
    std::vector<int> comboTriggerCodes;
    std::wstring triggerLabel;

    int centerXNorm = 500000;
    int centerYNorm = 500000;
    int leftNorm = 570000;
    int topNorm = 250000;
    int rightNorm = 960000;
    int bottomNorm = 850000;

    // 每个 RawInput 相对像素对应的客户端像素移动倍率。
    // 例如 speedX=0.5 表示鼠标相对移动 1px，触点在画面里移动 0.5px。
    double speedXNorm = 0.5;
    double speedYNorm = 0.5;
    int rebuildDownDelayMs = 5;
    int primarySlot = 2;
    int auxSlot = 9;
    PcLockSlideTouchMode mode = PcLockSlideTouchMode::DualSequential;
};

struct PcMappingBinding {
    std::string id;
    PcMappingTriggerType triggerType = PcMappingTriggerType::KeyboardVk;
    int triggerCode = 0;

    std::vector<int> comboTriggerCodes;
    std::wstring triggerLabel;

    PcMappingActionType actionType = PcMappingActionType::None;
    int slot = 1;
    int xNorm = 0;
    int yNorm = 0;
    int linuxKeyCode = 0;
    bool consumeEvent = true;

    // 统一按钮范围：overlay 显示为圆形，编辑命中、随机按下和随机移动都使用该半径。
    int radiusNorm = PC_BUTTON_RADIUS_DEFAULT_NORM;

    PcKeyTouchMode touchMode = PcKeyTouchMode::RandomMove;
    PcKeySpecialAction specialAction = PcKeySpecialAction::None;
    int randomRadiusNorm = 3200;
    int randomMoveRadiusNorm = 1800;
    int randomMoveIntervalMs = 16;
};

class PcMappingProfile final {
public:
    std::string name = "default";
    std::vector<PcMappingBinding> bindings;
    std::vector<PcLockBinding> locks;
    std::vector<PcCompassBinding> compasses;
    std::vector<PcMenuBinding> menus;

    void Clear();
    bool Empty() const;
    const PcMappingBinding* FindBinding(PcMappingTriggerType type, int code) const;

    static PcMappingProfile EmptyProfile();
    static int ClampNorm(int v);
    static int ClampRadiusNorm(int v);
    static int ClampButtonRadiusNorm(int v);

    bool SaveToFile(const std::wstring& path) const;
    bool LoadFromFile(const std::wstring& path, std::string* error = nullptr);
};
