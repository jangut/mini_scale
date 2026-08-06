"""网络天气 / 定位（免 API key，标准库实现）。

- 城市名 -> Open-Meteo geocoding（城市名转经纬度）-> 当前气温
- 公网 IP -> ip-api.com（返回城市/经纬度，可能受代理/出口 IP 影响而不准）

所有请求带超时，失败一律返回 None，绝不抛异常；UI 层负责兜底显示。
"""
import json
import urllib.parse
import urllib.request

TIMEOUT = 8  # 秒


def _get_json(url: str):
    req = urllib.request.Request(
        url, headers={"User-Agent": "MiniScale-GUI/1.0"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_city_temp(city: str):
    """按城市名获取当前气温。

    返回 (temp_c, label)；失败或城市无效返回 (None, None)。
    label 为 geocoding 返回的城市名（用于状态栏显示）。
    """
    city = (city or "").strip()
    if not city:
        return None, None
    try:
        q = urllib.parse.quote(city)
        geo = _get_json(
            "https://geocoding-api.open-meteo.com/v1/search"
            f"?name={q}&count=1&language=zh&format=json")
        results = geo.get("results") or []
        if not results:
            return None, None
        r0 = results[0]
        lat, lon = r0["latitude"], r0["longitude"]
        name = r0.get("name") or city
        fx = _get_json(
            "https://api.open-meteo.com/v1/forecast"
            f"?latitude={lat}&longitude={lon}&current=temperature_2m")
        temp = fx["current"]["temperature_2m"]
        return temp, name
    except Exception:
        return None, None


def fetch_ip_city():
    """按公网 IP 定位城市。

    返回 (city, lat, lon)；失败返回 (None, None, None)。
    注意：出口 IP 经过代理时定位可能不准。
    """
    try:
        info = _get_json(
            "http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon")
        if info.get("status") != "success":
            return None, None, None
        return info.get("city"), info.get("lat"), info.get("lon")
    except Exception:
        return None, None, None
