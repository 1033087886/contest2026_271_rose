# Xiao V Host Gateway

This is the host-side half of the Mode A application: WebSocket gateway,
replaceable speech providers, MiMo tool loop, Open-Meteo weather path,
MQTT/media/video integrations, Markdown Skills, and a protocol simulator.

```bash
python -m pip install -e ".[mimo,mqtt,speech]"
python -m gateway.server
python -m simulator.device --uri ws://127.0.0.1:8765
```

Production credentials must be supplied through environment variables or
private local files and must not be committed.
