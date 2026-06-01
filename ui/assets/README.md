# ui/assets — 静态资源

图片、字库等 **无业务逻辑** 的编译期资源。

## 当前文件

| 文件 | 说明 |
|------|------|
| `snowpod_device_img.h` / `.c` | 首页设备图 + 地面倒影（`lv_image_dsc_t`） |

由 `tools/gen_device_img.py` 生成；生成后输出到本目录：

```bash
# 示例（以项目脚本实际参数为准）
python tools/gen_device_img.py --out-dir ui/assets
```

## 使用

```cpp
#include "snowpod_device_img.h"

lv_image_set_src(img, &snowpod_device_img);
```

## 原则

- 仅 `.c` / `.h` 资源与生成脚本说明，不放页面布局代码
- 页面与 components 通过 `#include` 引用，assets 不反向依赖 pages
