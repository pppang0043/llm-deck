"""공급자 비용의 단위, 페이지네이션, UTC/월 범위를 네트워크 없이 검증한다."""

import io
import json
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlparse

import pytest

from collectors import anthropic_api, billing, budget, openai_api


def test_openai_cost_pages_and_usd_decimal(monkeypatch):
    calls = []
    start = int(datetime.now(timezone.utc).replace(day=1, hour=0, minute=0, second=0,
                                                  microsecond=0).timestamp())

    def get(path, params, key):
        calls.append(params)
        second = params.get("page") == "page2"
        return {"data": [{"start_time": start + (86400 if second else 0),
                           "results": [{"amount": {"value": "0.29110238", "currency": "usd"}}]}],
                "has_more": not second, "next_page": "page2" if not second else None}

    monkeypatch.setattr(openai_api, "_get", get)
    monkeypatch.setattr(openai_api.keys, "mark_ok", lambda *a: None)
    result = openai_api.fetch({"admin_key": "test", "monthly_limit": 10})
    assert result["month"] == pytest.approx(0.58220476)
    assert len(calls) == 2 and calls[1]["page"] == "page2"
    assert calls[0]["start_time"] == calls[1]["start_time"]


def test_anthropic_cost_pages_and_cents(monkeypatch):
    calls = []
    month = datetime.now(timezone.utc).strftime("%Y-%m")

    def urlopen(request, timeout):
        params = parse_qs(urlparse(request.full_url).query)
        calls.append(params)
        second = params.get("page") == ["page2"]
        body = {"data": [{"starting_at": month + ("-02" if second else "-01") + "T00:00:00Z",
                          "results": [{"amount": "29.110238"}]}],
                "has_more": not second, "next_page": "page2" if not second else None}
        return io.StringIO(json.dumps(body))

    monkeypatch.setattr(anthropic_api.urllib.request, "urlopen", urlopen)
    monkeypatch.setattr(anthropic_api.keys, "mark_ok", lambda *a: None)
    result = anthropic_api.fetch({"admin_key": "test"})
    assert result["month"] == pytest.approx(0.58220476)
    assert len(calls) == 2 and calls[1]["page"] == ["page2"]


def test_billing_month_is_utc_and_not_payment_receipt(monkeypatch):
    class Boundary(datetime):
        @classmethod
        def now(cls, tz=None):
            assert tz is timezone.utc
            return cls(2026, 10, 31, 23, 30, tzinfo=tz)

    monkeypatch.setattr(billing, "datetime", Boundary)
    monkeypatch.setattr(billing.anthropic_api, "fetch", lambda cfg: None)
    monkeypatch.setattr(billing.openai_api, "fetch", lambda cfg: {"month": 0.58, "today": 0.01})
    monkeypatch.setattr(billing, "_last", {})
    page = billing.collect({})
    assert page["table"][1] == ["10월", "$0.58"]
    assert page["note"] == "UTC 23:30"
    assert "결제 내역 아님" in page["sub"]


def test_credit_lookback_is_not_added_to_month():
    cfg = {"credit": 100, "credit_date": "2026-10-01"}
    month = datetime(2026, 11, 1, tzinfo=timezone.utc)
    today = datetime(2026, 11, 28, tzinfo=timezone.utc)
    result = budget.summary(cfg, {"2026-10-31": 50, "2026-11-01": 0.5, "2026-11-28": 0.08},
                            month, today)
    assert result["month"] == pytest.approx(0.58)
    assert result["today"] == pytest.approx(0.08)
    assert result["credit_left"] == pytest.approx(49.42)
