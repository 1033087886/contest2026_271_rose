/* Copyright 2026 Xiao V contributors; SPDX-License-Identifier: Apache-2.0 */

#include "xiaov_skills.h"

#include <stddef.h>

#ifdef __NuttX__
#  include <errno.h>
#  include <stdio.h>
#  include <sys/stat.h>
#endif

typedef struct {
  const char *name;
  const char *contents;
} xv_skill_file_t;

static const xv_skill_file_t g_skill_files[] = {
    {"weather.md",
     "---\nname: weather\ndescription: 查询指定城市的当前天气并用一句中文播报。\n---\n\n"
     "# 天气 Skill\n\n当用户询问一个明确城市的当前天气时，提取城市名并调用天气工具。\n"
     "回答只保留温度、体感温度、天气状况和湿度；不要编造实时数据。\n"
     "没有城市、涉及预报或包含其他操作时，交给通用 Agent 处理。\n"},
    {"smart-home.md",
     "---\nname: smart-home\ndescription: 将家居语音意图转换为受控 MQTT 设备动作。\n---\n\n"
     "# 智能家居 Skill\n\n只控制已配置并授权的设备。先识别设备、动作和参数，再调用工具。\n"
     "空调温度限制在 16 到 30 摄氏度；没有明确执行确认时，不要声称已经完成。\n"
     "网络或设备不可用时，清楚报告失败原因，不重试敏感操作。\n"},
};

#ifdef __NuttX__
static bool make_directory(const char *path) {
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return true;
  }
  printf("xiaov: skills mkdir failed path=%s errno=%d\n", path, errno);
  return false;
}
#endif

bool xv_skills_install(void) {
#ifdef __NuttX__
  const char *root = "/data/agent";
  const char *directory = "/data/agent/skills";
  if (!make_directory(root) || !make_directory(directory)) {
    return false;
  }
  for (size_t index = 0; index < sizeof(g_skill_files) / sizeof(g_skill_files[0]);
       ++index) {
    char path[96];
    FILE *file;
    int written;
    (void)snprintf(path, sizeof(path), "%s/%s", directory,
                   g_skill_files[index].name);
    file = fopen(path, "w");
    if (file == NULL) {
      printf("xiaov: skills open failed name=%s errno=%d\n",
             g_skill_files[index].name, errno);
      return false;
    }
    written = fputs(g_skill_files[index].contents, file);
    if (written < 0 || fclose(file) != 0) {
      printf("xiaov: skills write failed name=%s\n", g_skill_files[index].name);
      return false;
    }
  }
  printf("xiaov: skills installed count=%u path=%s\n",
         (unsigned)(sizeof(g_skill_files) / sizeof(g_skill_files[0])), directory);
  return true;
#else
  return true;
#endif
}
