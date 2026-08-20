# AIagent
## 项目简介
本项目是基于Qt6开发的多模态AI智能应用。
集成本地Ollama大模型，实现三大能力：
1. 普通文本多轮对话，支持流式打字机输出；
2. VLM图像理解：上传本地图片，AI识别图片内容并回答图片相关问题；
3. AI Agent工具调用：模型可自动调用本地工具，包含数学计算器、获取系统当前时间。

本项目全部为本地离线推理，不需要联网API密钥。

## 开发环境
操作系统：Windows 11
Qt版本：Qt 6.x MSVC2022 64‑bit
第三方服务：Ollama本地大模型服务
使用模型：qwen2.5‑vl:2b

## 前置运行准备
1. 电脑安装 Ollama，确认托盘Ollama后台服务已启动，默认服务地址：`http://127.0.0.1:11434`
2. PowerShell执行命令下载模型：
```powershell
ollama pull qwen2.5-vl:7b
