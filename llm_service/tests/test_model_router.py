"""Tests for ModelRouter - multi-provider routing with health tracking."""

import pytest
from unittest.mock import AsyncMock, MagicMock, patch
from model_router import ModelRouter, ProviderHealth


@pytest.fixture
def priority_config():
    return {
        "strategy": "priority",
        "models": [
            {
                "name": "primary",
                "provider": "openai",
                "api_key": "test-key",
                "base_url": "https://api.test.com/v1",
                "model": "test-model",
                "max_tokens": 100,
                "temperature": 0.3,
                "timeout": 3.0,
                "priority": 1,
                "tier": "lightweight",
                "enabled": True,
            },
            {
                "name": "backup",
                "provider": "ollama",
                "api_key": "",
                "base_url": "http://localhost:11434",
                "model": "llama3.2",
                "max_tokens": 80,
                "temperature": 0.3,
                "timeout": 45.0,
                "priority": 2,
                "tier": "lightweight",
                "enabled": True,
            },
        ]
    }


@pytest.fixture
def legacy_config():
    return {
        "strategy": "priority",
        "llm": {
            "provider": "openai",
            "api_key": "legacy-key",
            "base_url": "https://api.legacy.com/v1",
            "model": "legacy-model",
        }
    }


class TestProviderHealth:
    def test_initial_state(self):
        h = ProviderHealth()
        assert h.is_healthy()
        assert h.success_count == 0
        assert h.fail_count == 0

    def test_success_tracking(self):
        h = ProviderHealth()
        h.record_success(2.5)
        assert h.success_count == 1
        assert h.avg_latency == 2.5

    def test_failure_tracking(self):
        h = ProviderHealth()
        h.record_failure()
        assert h.fail_count == 1
        # Should be unhealthy due to recent failure (within 10s cooldown)
        assert not h.is_healthy()

    def test_recovery_after_cooldown(self):
        h = ProviderHealth()
        h.record_failure()
        assert not h.is_healthy()
        # Simulate cooldown passed
        h.last_fail_time = 0.0  # Set to epoch
        assert h.is_healthy()

    def test_permanently_unhealthy(self):
        h = ProviderHealth()
        for _ in range(6):
            h.record_failure()
        # 6 failures, 0 successes → permanently unhealthy
        assert not h.is_healthy()

    def test_avg_latency_rolling(self):
        h = ProviderHealth()
        for lat in [1.0, 2.0, 3.0]:
            h.record_success(lat)
        assert h.avg_latency == pytest.approx(2.0)

    def test_latency_window(self):
        h = ProviderHealth()
        for i in range(25):
            h.record_success(float(i))
        # Only last 20 latencies kept
        assert len(h._latencies) == 20


class TestModelRouterInit:
    def test_load_from_models_list(self, priority_config):
        router = ModelRouter(priority_config)
        assert len(router.providers) == 2
        assert router.providers[0]["name"] == "primary"
        assert router.providers[1]["name"] == "backup"
        assert router.strategy == "priority"

    def test_fallback_to_legacy_config(self, legacy_config):
        router = ModelRouter(legacy_config)
        assert len(router.providers) == 1
        assert router.providers[0]["name"] == "provider_0"

    def test_empty_config(self):
        router = ModelRouter({})
        assert len(router.providers) == 0
        assert router.strategy == "priority"


class TestRoutingStrategy:
    def test_priority_order(self, priority_config):
        router = ModelRouter(priority_config)
        order = router._route_priority()
        assert order[0]["name"] == "primary"
        assert order[1]["name"] == "backup"

    def test_round_robin_rotation(self, priority_config):
        router = ModelRouter(priority_config)
        # First call
        order1 = router._route_round_robin()
        first_name = order1[0]["name"]
        # Second call should rotate
        order2 = router._route_round_robin()
        second_name = order2[0]["name"]
        assert first_name != second_name

    def test_adaptive_low_complexity(self, priority_config):
        router = ModelRouter(priority_config)
        # No threats → low complexity → lightweight first
        state = {"threats": [], "teammates": [], "bot": {"hp": 100}}
        order = router._route_adaptive(state)
        assert order[0]["tier"] == "lightweight"

    def test_adaptive_high_complexity(self, priority_config):
        router = ModelRouter(priority_config)
        # Many threats → high complexity
        state = {
            "threats": [{"type": "tank", "ghost": False}, {"type": "hunter", "ghost": False},
                        {"type": "smoker", "ghost": False}, {"type": "charger", "ghost": False}],
            "teammates": [{"pinned": True}, {"pinned": True}],
            "bot": {"hp": 20}
        }
        complexity = router._assess_complexity(state)
        assert complexity > 0.5


class TestComplexityAssessment:
    def test_no_threats(self, priority_config):
        router = ModelRouter(priority_config)
        state = {"threats": [], "teammates": [], "bot": {"hp": 100}}
        assert router._assess_complexity(state) == 0.0

    def test_single_threat(self, priority_config):
        router = ModelRouter(priority_config)
        state = {"threats": [{"type": "hunter", "ghost": False}], "teammates": [], "bot": {"hp": 100}}
        c = router._assess_complexity(state)
        assert 0 < c < 0.5

    def test_ghost_threats_dont_count(self, priority_config):
        router = ModelRouter(priority_config)
        state = {"threats": [{"type": "hunter", "ghost": True}], "teammates": [], "bot": {"hp": 100}}
        c = router._assess_complexity(state)
        assert c == 0.0

    def test_pinned_teammates(self, priority_config):
        router = ModelRouter(priority_config)
        state = {
            "threats": [],
            "teammates": [{"pinned": True}, {"pinned": True}],
            "bot": {"hp": 100}
        }
        c = router._assess_complexity(state)
        assert c >= 0.3

    def test_low_hp(self, priority_config):
        router = ModelRouter(priority_config)
        state = {"threats": [], "teammates": [], "bot": {"hp": 20}}
        c = router._assess_complexity(state)
        assert c > 0.0

    def test_capped_at_one(self, priority_config):
        router = ModelRouter(priority_config)
        state = {
            "threats": [{"type": f"t{i}", "ghost": False} for i in range(10)],
            "teammates": [{"pinned": True} for _ in range(5)],
            "bot": {"hp": 10}
        }
        c = router._assess_complexity(state)
        assert c <= 1.0


class TestTierOrder:
    def test_low_complexity(self):
        order = ModelRouter._tier_order_for_complexity(0.1)
        assert order[0] == "lightweight"

    def test_medium_complexity(self):
        order = ModelRouter._tier_order_for_complexity(0.4)
        assert order[0] == "standard"

    def test_high_complexity(self):
        order = ModelRouter._tier_order_for_complexity(0.8)
        assert order[0] == "premium"


class TestGetStatus:
    def test_status_format(self, priority_config):
        router = ModelRouter(priority_config)
        status = router.get_status()
        assert len(status) == 2
        assert "name" in status[0]
        assert "healthy" in status[0]
        assert "success" in status[0]
        assert "fail" in status[0]
        assert status[0]["name"] == "primary"
