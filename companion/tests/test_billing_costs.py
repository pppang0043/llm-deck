"""공급자 비용의 단위, 페이지네이션, UTC/월 범위를 네트워크 없이 검증한다."""

import io
import json
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlparse

import pytest

from collectors import anthropic_api, billing, budget, openai_api


def test_only_provider_costs_registered_and_missing_keys_not_zero(monkeypatch):
    from collectors import COLLECTORS
    assert [section for section, _, _ in COLLECTORS] == ["billing"]
    page = billing.collect({"api_cost": {"enabled": True}})
    assert page["table"][1][1:] == ["미설정", "미설정"]
    assert "headline" not in page and "side" not in page


def test_failed_provider_does_not_reuse_success_or_form_partial_total(monkeypatch):
    from collectors import provider_costs
    monkeypatch.setattr(anthropic_api, "fetch", lambda cfg: {"status": "auth", "error": "private diagnostic"})
    monkeypatch.setattr(openai_api, "fetch", lambda cfg: provider_costs.summary({}, datetime.now(timezone.utc)))
    first = billing.collect({})
    assert first["table"][1][1:] == ["인증필요", "$0.00"]
    assert first["table"][2][2] == "집계대기"
    def fail(cfg):
        raise OSError("private diagnostic")
    monkeypatch.setattr(openai_api, "fetch", fail)
    second = billing.collect({})
    assert second["table"][1][1:] == ["인증필요", "조회불가"]
    assert "$0.00" not in str(second) and "headline" not in second
    assert "private diagnostic" not in str(second)


def test_unsupported_currency_and_malformed_provider_data_are_not_zero(monkeypatch):
    get_all = openai_api._get_all
    monkeypatch.setattr(openai_api.keys, "mark_ok", lambda *a: None)
    monkeypatch.setattr(openai_api, "_get_all", lambda *a: [{"start_time": 0, "results": [
        {"amount": {"currency": "eur", "value": "100"}}]}])
    page = billing.collect({"openai_api": {"admin_key": "test"}})
    assert page["table"][1][2] == "조회불가"
    monkeypatch.setattr(openai_api, "_get", lambda *a: {"unsupported": True})
    with pytest.raises(KeyError):
        get_all("costs", {}, "test")


def test_main_cannot_fallback_to_old_bill_on_collector_failure(monkeypatch):
    import main
    from types import SimpleNamespace
    monkeypatch.setattr(main, "collect_limits", lambda cfg: [])
    monkeypatch.setattr(main, "_sync", SimpleNamespace(public_url=None))
    monkeypatch.setattr(main, "_orca_accounts", [])
    monkeypatch.setattr(main, "_last", {"billing": (0, {"headline": "$1,031.38"})})
    def fail(cfg):
        raise OSError("unavailable")
    monkeypatch.setattr(billing, "collect", fail)
    result = main.collect_all({})
    assert "$1,031.38" not in str(result)
    assert result["services"][0]["table"][1][1:] == ["조회불가", "조회불가"]


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
        assert urlparse(request.full_url).path.endswith("/analytics/cost")
        assert params["products[]"] == list(anthropic_api.API_PRODUCTS)
        assert params["group_by[]"] == ["product"]
        assert params["time_bucket_width"] == ["1d"]
        headers = {k.lower(): v for k, v in request.header_items()}
        assert headers["anthropic-version"] == "2026-10-21"
        assert headers["anthropic-beta"] == "analytics-usage-cost-2026-08-24"
        calls.append(params)
        second = params.get("page") == ["page2"]
        body = {"data": [{"starting_at": month + "-01T00:00:00Z",
                          "group": {"product": "message_batches_api" if second else "messages_api"},
                          "cost": {"amount": "29.110238", "currency": "USD"}}],
                "has_more": not second, "next_page": "page2" if not second else None}
        return io.StringIO(json.dumps(body))

    monkeypatch.setattr(anthropic_api.urllib.request, "urlopen", urlopen)
    monkeypatch.setattr(anthropic_api.keys, "mark_ok", lambda *a: None)
    result = anthropic_api.fetch({"admin_key": "test"})
    assert result["month"] == pytest.approx(0.58220476)
    assert len(calls) == 2 and calls[1]["page"] == ["page2"]


@pytest.mark.parametrize("product", ["claude_code", "cowork", None])
def test_anthropic_non_api_product_cannot_enter_cost_page(monkeypatch, product):
    paths = []
    def get_all(path, params, key):
        paths.append(path)
        return [{"starting_at": "2026-11-01T00:00:00Z", "group": {"product": product},
                 "cost": {"amount": "100", "currency": "USD"}}]
    monkeypatch.setattr(anthropic_api, "_get_all", get_all)
    page = billing.collect({"anthropic_api": {"admin_key": "test"}})
    assert page["table"][1][1] == "조회불가"
    assert paths == ["analytics/cost"]  # legacy/local fallback은 호출하지 않는다.


def test_anthropic_missing_export_not_zero(monkeypatch):
    monkeypatch.setattr(anthropic_api.urllib.request, "urlopen", lambda *a, **k: io.StringIO(json.dumps({
        "data": [], "has_more": False, "next_page": None, "data_refreshed_at": None})))
    page = billing.collect({"anthropic_api": {"admin_key": "test"}})
    assert page["table"][1][1] == "조회불가"


def test_billing_month_is_utc_and_not_payment_receipt(monkeypatch):
    class Boundary(datetime):
        @classmethod
        def now(cls, tz=None):
            assert tz is timezone.utc
            return cls(2026, 10, 31, 23, 30, tzinfo=tz)

    monkeypatch.setattr(billing, "datetime", Boundary)
    monkeypatch.setattr(billing.anthropic_api, "fetch", lambda cfg: None)
    monkeypatch.setattr(billing.openai_api, "fetch", lambda cfg: {
        "month": 0.58, "today": 0.01, "currency": "USD", "scope": "organization",
        "month_period": "2026-10", "day_period": "2026-10-31"})
    page = billing.collect({})
    assert page["table"][1] == ["2026-10", "미설정", "$0.58"]
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
