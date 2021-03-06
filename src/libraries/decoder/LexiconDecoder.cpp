/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>

#include "libraries/decoder/LexiconDecoder.h"

namespace w2l {

void LexiconDecoder::candidatesReset() {
  candidatesBestScore_ = kNegativeInfinity;
  candidates_.clear();
  candidatePtrs_.clear();
}

void LexiconDecoder::mergeCandidates() {
  auto compareNodesShortList = [](const LexiconDecoderState* node1,
                                  const LexiconDecoderState* node2) {
    int lmCmp = node1->lmState->compare(node2->lmState);
    if (lmCmp != 0) {
      return lmCmp > 0;
    } else if (node1->lex != node2->lex) {
      /* same LmState */
      return node1->lex > node2->lex;
    } else if (node1->token != node2->token) {
      return node1->token > node2->token;
    } else if (node1->prevBlank != node2->prevBlank) {
      return node1->prevBlank > node2->prevBlank;
    } else {
      /* same LmState, same lex */
      return node1->score > node2->score;
    }
  };
  std::sort(
      candidatePtrs_.begin(), candidatePtrs_.end(), compareNodesShortList);

  int nHypAfterMerging = 1;
  for (int i = 1; i < candidatePtrs_.size(); i++) {
    if (candidatePtrs_[i]->lmState->compare(
            candidatePtrs_[nHypAfterMerging - 1]->lmState) ||
        candidatePtrs_[i]->lex != candidatePtrs_[nHypAfterMerging - 1]->lex ||
        candidatePtrs_[i]->token !=
            candidatePtrs_[nHypAfterMerging - 1]->token ||
        candidatePtrs_[i]->prevBlank !=
            candidatePtrs_[nHypAfterMerging - 1]->prevBlank) {
      candidatePtrs_[nHypAfterMerging] = candidatePtrs_[i];
      nHypAfterMerging++;
    } else {
      mergeStates(
          candidatePtrs_[nHypAfterMerging - 1], candidatePtrs_[i], opt_.logAdd);
    }
  }

  candidatePtrs_.resize(nHypAfterMerging);
}

void LexiconDecoder::candidatesAdd(
    const LMStatePtr& lmState,
    const TrieNode* lex,
    const LexiconDecoderState* parent,
    const double score,
    const int token,
    const int word,
    const bool prevBlank) {
  if (isValidCandidate(candidatesBestScore_, score, opt_.beamThreshold)) {
    candidates_.emplace_back(
        lmState, lex, parent, score, token, word, prevBlank);
  }
}

void LexiconDecoder::candidatesStore(
    std::vector<LexiconDecoderState>& nextHyp,
    const bool returnSorted) {
  if (candidates_.empty()) {
    nextHyp.clear();
    return;
  }

  /* Select valid candidates */
  pruneCandidates(
      candidatePtrs_, candidates_, candidatesBestScore_ - opt_.beamThreshold);

  /* Sort by (lmState, lex, score) and copy into next hypothesis */
  mergeCandidates();

  /* Sort hypothesis and select top-K */
  storeTopCandidates(nextHyp, candidatePtrs_, opt_.beamSize, returnSorted);
}

void LexiconDecoder::decodeBegin() {
  hyp_.clear();
  hyp_.emplace(0, std::vector<LexiconDecoderState>());

  /* note: the lm reset itself with :start() */
  hyp_[0].emplace_back(
      lm_->start(0), lexicon_->getRoot(), nullptr, 0.0, sil_, -1);
  nDecodedFrames_ = 0;
  nPrunedFrames_ = 0;
}

void LexiconDecoder::decodeStep(const float* emissions, int T, int N) {
  int startFrame = nDecodedFrames_ - nPrunedFrames_;
  // Extend hyp_ buffer
  if (hyp_.size() < startFrame + T + 2) {
    for (int i = hyp_.size(); i < startFrame + T + 2; i++) {
      hyp_.emplace(i, std::vector<LexiconDecoderState>());
    }
  }

  std::vector<size_t> idx(N);
  for (int t = 0; t < T; t++) {
    std::iota(idx.begin(), idx.end(), 0);
    if (N > opt_.beamSizeToken) {
      std::partial_sort(
          idx.begin(),
          idx.begin() + opt_.beamSizeToken,
          idx.end(),
          [&t, &N, &emissions](const size_t& l, const size_t& r) {
            return emissions[t * N + l] > emissions[t * N + r];
          });
    }

    candidatesReset();
    for (const LexiconDecoderState& prevHyp : hyp_[startFrame + t]) {
      const TrieNode* prevLex = prevHyp.lex;
      const int prevIdx = prevHyp.token;
      const float lexMaxScore =
          prevLex == lexicon_->getRoot() ? 0 : prevLex->maxScore;

      /* (1) Try children */
      for (int r = 0; r < std::min(opt_.beamSizeToken, N); ++r) {
        int n = idx[r];
        auto iter = prevLex->children.find(n);
        if (iter == prevLex->children.end()) {
          continue;
        }
        const TrieNodePtr& lex = iter->second;
        double score = prevHyp.score + emissions[t * N + n];
        if (nDecodedFrames_ + t > 0 &&
            opt_.criterionType == CriterionType::ASG) {
          score += transitions_[n * N + prevIdx];
        }
        if (n == sil_) {
          score += opt_.silScore;
        }

        LMStatePtr lmState;
        double lmScore = 0.;

        if (isLmToken_) {
          auto lmReturn = lm_->score(prevHyp.lmState, n);
          lmState = lmReturn.first;
          lmScore = lmReturn.second;
        }

        // We eat-up a new token
        if (opt_.criterionType != CriterionType::CTC || prevHyp.prevBlank ||
            n != prevIdx) {
          if (!lex->children.empty()) {
            if (!isLmToken_) {
              lmState = prevHyp.lmState;
              lmScore = lex->maxScore - lexMaxScore;
            }
            candidatesAdd(
                lmState,
                lex.get(),
                &prevHyp,
                score + opt_.lmWeight * lmScore,
                n,
                -1,
                false // prevBlank
            );
          }
        }

        // If we got a true word
        for (auto label : lex->labels) {
          if (!isLmToken_) {
            auto lmReturn = lm_->score(prevHyp.lmState, label);
            lmState = lmReturn.first;
            lmScore = lmReturn.second - lexMaxScore;
          }
          candidatesAdd(
              lmState,
              lexicon_->getRoot(),
              &prevHyp,
              score + opt_.lmWeight * lmScore + opt_.wordScore,
              n,
              label,
              false // prevBlank
          );
        }

        // If we got an unknown word
        if (lex->labels.empty() && (opt_.unkScore > kNegativeInfinity)) {
          if (!isLmToken_) {
            auto lmReturn = lm_->score(prevHyp.lmState, unk_);
            lmState = lmReturn.first;
            lmScore = lmReturn.second - lexMaxScore;
          }
          candidatesAdd(
              lmState,
              lexicon_->getRoot(),
              &prevHyp,
              score + opt_.lmWeight * lmScore + opt_.unkScore,
              n,
              unk_,
              false // prevBlank
          );
        }
      }

      /* (2) Try same lexicon node */
      if (opt_.criterionType != CriterionType::CTC || !prevHyp.prevBlank) {
        int n = prevIdx;
        double score = prevHyp.score + emissions[t * N + n];
        if (nDecodedFrames_ + t > 0 &&
            opt_.criterionType == CriterionType::ASG) {
          score += transitions_[n * N + prevIdx];
        }
        if (n == sil_) {
          score += opt_.silScore;
        }

        candidatesAdd(
            prevHyp.lmState,
            prevLex,
            &prevHyp,
            score,
            n,
            -1,
            false // prevBlank
        );
      }

      /* (3) CTC only, try blank */
      if (opt_.criterionType == CriterionType::CTC) {
        int n = blank_;
        double score = prevHyp.score + emissions[t * N + n];
        candidatesAdd(
            prevHyp.lmState,
            prevLex,
            &prevHyp,
            score,
            n,
            -1,
            true // prevBlank
        );
      }
      // finish proposing
    }

    candidatesStore(hyp_[startFrame + t + 1], false);
    updateLMCache(lm_, hyp_[startFrame + t + 1]);
  }

  nDecodedFrames_ += T;
}

void LexiconDecoder::decodeEnd() {
  candidatesReset();
  bool hasNiceEnding = false;
  for (const LexiconDecoderState& prevHyp :
       hyp_[nDecodedFrames_ - nPrunedFrames_]) {
    if (prevHyp.lex == lexicon_->getRoot()) {
      hasNiceEnding = true;
      break;
    }
  }
  for (const LexiconDecoderState& prevHyp :
       hyp_[nDecodedFrames_ - nPrunedFrames_]) {
    const TrieNode* prevLex = prevHyp.lex;
    const LMStatePtr& prevLmState = prevHyp.lmState;

    if (!hasNiceEnding || prevHyp.lex == lexicon_->getRoot()) {
      auto lmStateScorePair = lm_->finish(prevLmState);
      candidatesAdd(
          lmStateScorePair.first,
          prevLex,
          &prevHyp,
          prevHyp.score + opt_.lmWeight * lmStateScorePair.second,
          sil_,
          -1,
          false // prevBlank
      );
    }
  }

  candidatesStore(hyp_[nDecodedFrames_ - nPrunedFrames_ + 1], true);
  ++nDecodedFrames_;
}

std::vector<DecodeResult> LexiconDecoder::getAllFinalHypothesis() const {
  int finalFrame = nDecodedFrames_ - nPrunedFrames_;
  if (finalFrame < 1) {
    return std::vector<DecodeResult>{};
  }

  return getAllHypothesis(hyp_.find(finalFrame)->second, finalFrame);
}

DecodeResult LexiconDecoder::getBestHypothesis(int lookBack) const {
  if (nDecodedFrames_ - nPrunedFrames_ - lookBack < 1) {
    return DecodeResult();
  }

  const LexiconDecoderState* bestNode = findBestAncestor(
      hyp_.find(nDecodedFrames_ - nPrunedFrames_)->second, lookBack);
  return getHypothesis(bestNode, nDecodedFrames_ - nPrunedFrames_ - lookBack);
}

int LexiconDecoder::nHypothesis() const {
  int finalFrame = nDecodedFrames_ - nPrunedFrames_;
  return hyp_.find(finalFrame)->second.size();
}

int LexiconDecoder::nDecodedFramesInBuffer() const {
  return nDecodedFrames_ - nPrunedFrames_ + 1;
}

void LexiconDecoder::prune(int lookBack) {
  if (nDecodedFrames_ - nPrunedFrames_ - lookBack < 1) {
    return; // Not enough decoded frames to prune
  }

  /* (1) Find the last emitted word in the best path */
  const LexiconDecoderState* bestNode = findBestAncestor(
      hyp_.find(nDecodedFrames_ - nPrunedFrames_)->second, lookBack);
  if (!bestNode) {
    return; // Not enough decoded frames to prune
  }

  int startFrame = nDecodedFrames_ - nPrunedFrames_ - lookBack;
  if (startFrame < 1) {
    return; // Not enough decoded frames to prune
  }

  /* (2) Move things from back of hyp_ to front and normalize scores */
  pruneAndNormalize(hyp_, startFrame, lookBack);

  nPrunedFrames_ = nDecodedFrames_ - lookBack;
}

} // namespace w2l
