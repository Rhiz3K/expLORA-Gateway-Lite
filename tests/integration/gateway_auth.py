"""Cookie authentication shared by hardware regression tests."""

import requests


def ensure_login(session: requests.Session, base: str, password: str) -> None:
    if not password:
        raise RuntimeError("An administrator password is required")
    session.cookies.clear()
    response = session.post(
        base.rstrip("/") + "/login",
        data={"password": password},
        allow_redirects=False,
        timeout=15,
    )
    if response.status_code not in (302, 303) or not any(
        cookie.name == "explora_session" and cookie.value
        for cookie in session.cookies
    ):
        raise RuntimeError(f"Administrator login failed (HTTP {response.status_code})")
