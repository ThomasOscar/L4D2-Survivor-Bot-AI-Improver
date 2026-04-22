"""Tests for PlayerScorer - five-dimensional player scoring."""

import pytest
from player_scorer import PlayerScorer


@pytest.fixture
def scorer():
    return PlayerScorer()


class TestScorePlayer:
    def test_default_zero_score(self, scorer):
        player = {"name": "TestPlayer"}
        result = scorer.score_player(player)
        assert result["composite"] == 0.0
        assert result["rating"] == "C"

    def test_heal_positive(self, scorer):
        player = {"name": "Medic", "heal": 3}
        result = scorer.score_player(player)
        assert result["composite"] == 9.0  # 3 * 3.0
        assert result["rating"] == "A"

    def test_revive_positive(self, scorer):
        player = {"name": "Savior", "revive": 2}
        result = scorer.score_player(player)
        assert result["composite"] == 5.0  # 2 * 2.5
        assert result["rating"] == "A"

    def test_si_kills_positive(self, scorer):
        player = {"name": "Killer", "si_kills": 4}
        result = scorer.score_player(player)
        assert result["composite"] == 6.0  # 4 * 1.5
        assert result["rating"] == "A"

    def test_incap_negative(self, scorer):
        player = {"name": "Weak", "incap": 2}
        result = scorer.score_player(player)
        assert result["composite"] == -4.0  # 2 * -2.0
        assert result["rating"] == "D"

    def test_pinned_negative(self, scorer):
        player = {"name": "Pinned", "pinned": 3}
        result = scorer.score_player(player)
        assert result["composite"] == -3.0  # 3 * -1.0
        assert result["rating"] == "D"

    def test_mixed_score(self, scorer):
        player = {"name": "Mixed", "heal": 2, "revive": 1, "si_kills": 3, "incap": 1, "pinned": 2}
        # 2*3.0 + 1*2.5 + 3*1.5 + 1*-2.0 + 2*-1.0 = 6+2.5+4.5-2-2 = 9.0
        result = scorer.score_player(player)
        assert result["composite"] == 9.0
        assert result["rating"] == "A"

    def test_s_rating(self, scorer):
        player = {"name": "MVP", "heal": 4, "si_kills": 5}
        # 4*3.0 + 5*1.5 = 12+7.5 = 19.5
        result = scorer.score_player(player)
        assert result["composite"] >= 10
        assert result["rating"] == "S"

    def test_b_rating(self, scorer):
        player = {"name": "Average", "si_kills": 2}
        # 2*1.5 = 3.0
        result = scorer.score_player(player)
        assert result["composite"] >= 2
        assert result["rating"] == "B"

    def test_d_rating(self, scorer):
        player = {"name": "Liability", "incap": 3, "pinned": 2}
        # 3*-2.0 + 2*-1.0 = -8.0
        result = scorer.score_player(player)
        assert result["composite"] < 0
        assert result["rating"] == "D"


class TestScoreTeam:
    def test_empty_team(self, scorer):
        result = scorer.score_team([])
        assert result["team_score"] == 0
        assert result["team_rating"] == "C"
        assert result["mvp"] is None
        assert result["vulnerable_players"] == []
        assert result["needs_protection"] == []

    def test_team_with_scorers(self, scorer):
        players = [
            {"name": "Star", "heal": 3, "si_kills": 5},
            {"name": "Newbie", "incap": 2},
        ]
        scored = [scorer.score_player(p) for p in players]
        result = scorer.score_team(scored)
        assert result["mvp"] == "Star"
        assert "Newbie" in result["needs_protection"]
        assert result["team_score"] > 0

    def test_team_all_negative(self, scorer):
        players = [
            {"name": "P1", "incap": 2},
            {"name": "P2", "incap": 3, "pinned": 2},
        ]
        scored = [scorer.score_player(p) for p in players]
        result = scorer.score_team(scored)
        assert result["team_rating"] == "D"
        assert result["mvp"] is None

    def test_vulnerable_players(self, scorer):
        players = [
            {"name": "Weak", "incap": 3},
            {"name": "Strong", "heal": 2},
        ]
        scored = [scorer.score_player(p) for p in players]
        result = scorer.score_team(scored)
        assert "Weak" in result["vulnerable_players"]


class TestDecisionHint:
    def test_no_scores_no_hint(self, scorer):
        state = {"bot": {"name": "Nick"}}
        assert scorer.get_decision_hint(state) is None

    def test_struggling_team_hint(self, scorer):
        state = {
            "bot": {"name": "Nick"},
            "player_scores": [
                {"name": "Nick", "incap": 3, "pinned": 2},
            ]
        }
        hint = scorer.get_decision_hint(state)
        assert hint is not None
        assert "struggling" in hint.lower()

    def test_needs_protection_hint(self, scorer):
        state = {
            "bot": {"name": "Nick"},
            "player_scores": [
                {"name": "Nick", "heal": 1},
                {"name": "Rochelle", "incap": 2},
            ]
        }
        hint = scorer.get_decision_hint(state)
        assert hint is not None
        assert "Rochelle" in hint

    def test_bot_is_weakest_hint(self, scorer):
        state = {
            "bot": {"name": "Nick"},
            "player_scores": [
                {"name": "Nick", "incap": 3},
                {"name": "Rochelle", "heal": 2},
            ]
        }
        hint = scorer.get_decision_hint(state)
        assert hint is not None
        assert "weakest" in hint.lower() or "support" in hint.lower()

    def test_healer_role_hint(self, scorer):
        state = {
            "bot": {"name": "Nick"},
            "player_scores": [
                {"name": "Nick", "heal": 3},
            ]
        }
        hint = scorer.get_decision_hint(state)
        assert hint is not None
        assert "healer" in hint.lower()


class TestScoreSummary:
    def test_no_scores(self, scorer):
        state = {}
        summary = scorer.get_score_summary(state)
        assert "No player scores" in summary

    def test_with_scores(self, scorer):
        state = {
            "player_scores": [
                {"name": "Nick", "heal": 2, "revive": 1, "si_kills": 3, "incap": 0, "pinned": 0},
            ]
        }
        summary = scorer.get_score_summary(state)
        assert "Nick" in summary
        assert "Team Score" in summary
