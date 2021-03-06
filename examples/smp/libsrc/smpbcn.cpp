// --------------------------------------------
// Copyright KAPSARC. Open source MIT License.
// --------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2015 King Abdullah Petroleum Studies and Research Center
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom
// the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// --------------------------------------------
//
// Demonstrate a very basic, but highly parameterizable, Spatial Model of Politics.
//
// --------------------------------------------

#include "smp.h"
#include <QSqlQuery>
#include <QVariant>
#include <QSqlError>

namespace SMPLib {
using std::function;
using std::get;
using std::string;
using std::map;

using KBase::PRNG;
using KBase::KMatrix;
using KBase::KException;
using KBase::Actor;
using KBase::Model;
using KBase::Position;
using KBase::VctrPstn;
using KBase::BigRAdjust;
using KBase::BigRRange;
using KBase::VPModel;
using KBase::State;
using KBase::StateTransMode;
using KBase::VotingRule;
using KBase::PCEModel;
using KBase::ReportingLevel;
using KBase::nameFromEnum;

// --------------------------------------------
uint64_t BargainSMP::highestBargainID = 1000;

// big enough buffer to build all desired SQLite statements
const unsigned int sqlBuffSize = 250;

// --------------------------------------------
ostream& operator<< (ostream& os, const SMPBargnModel& bMod) {
  string s = nameFromEnum<SMPBargnModel>(bMod, SMPBargnModelNames);
  os << s;
  return os;
}

ostream& operator<< (ostream& os, const InterVecBrgn& ivb) {
  string s = nameFromEnum<InterVecBrgn>(ivb, InterVecBrgnNames);
  os << s;
  return os;
}

// --------------------------------------------

BargainSMP::BargainSMP(const SMPActor* ai, const SMPActor* ar, const VctrPstn & pi, const VctrPstn & pr) {
  assert(nullptr != ai);
  assert(nullptr != ar);
  actInit = ai;
  actRcvr = ar;
  posInit = pi;
  posRcvr = pr;
  myBargainID = BargainSMP::highestBargainID++;
}

BargainSMP::~BargainSMP() {
  actInit = nullptr;
  actRcvr = nullptr;
  posInit = VctrPstn(KMatrix(0, 0));
  posRcvr = VctrPstn(KMatrix(0, 0));
  auto mbid = myBargainID;
  myBargainID = 0;
}


uint64_t BargainSMP::getID() const {
  return myBargainID;
}

// --------------------------------------------
/*
 * Calculate all the utilities and record in database. utitlity for (i,i,i,j)
 * combination is getting calculated and recorded in a separate method
 */
void SMPState::calcUtils(unsigned int i, unsigned int bestJ ) const { // i == actor id
  const unsigned int na = model->numAct;
  const bool recordTmpSQLP = true;  // Record this in SQLite
  auto pFn = [this, recordTmpSQLP](unsigned int h, unsigned int k, unsigned int i, unsigned int j) {
    probEduChlg(h, k, i, j, recordTmpSQLP); // H's estimate of the effect on K of I->J
  };

  auto getUtils = [this, na, pFn, i](unsigned int j) {
    if( i != j ) {
      for (unsigned int m = 0; m < na; m++) {
        if(m == i) {
          /* Note:
           * pFn(i, i, i, j) = pFn(m, m, i, j) = pFn(m,i,i,j)
           * pFn(i, i, i, j) would be calculated in a different method
           * bestChallengeUtils in the main thread so that the required
           * utilities are ready before the call to bestChallenge()
           */
          pFn(m, j, i, j); // m's estimate of the effect on J of I->J
        } else if (m == j) {
          pFn(m, i, i, j); // m's estimate of the effect on I of I->J
          pFn(m, m, i, j); // pFn(m,j,i,j) would produce a duplicate result
        } else {
          pFn(m, i, i, j); // m's estimate of the effect on I of I->J
          pFn(m, m, i, j); // m's estimate of the effect on I of I->J
          pFn(m, j, i, j); // m's estimate of the effect on J of I->J
        }
      }
    }
  };

  for (unsigned int j = 0; j < bestJ; j++) {
    getUtils(j);
  }

  // Some computes for best J need to be avoided here to prevent duplicate entries in DB
  if( i != bestJ ) {
    for (unsigned int m = 0; m < na; m++) {
      if((m != i) && (m != bestJ)) {
        pFn(m, i, i, bestJ); // m's estimate of the effect on I of I->J
        pFn(m, m, i, bestJ); // m's estimate of the effect on I of I->J
        pFn(m, bestJ, i, bestJ); // m's estimate of the effect on J of I->J
      }
    }
  }

  for (unsigned int j = bestJ+1; j < na; j++) {
    getUtils(j);
  }
}

// --------------------------------------------
eduChlgsI SMPState::bestChallengeUtils(unsigned int i) const {
  const unsigned int na = model->numAct;
  const bool recordTmpSQLP = true;  // Record this in SQLite
  eduChlgsI eduI;
  for (unsigned int j = 0; j < na; j++) {
    if( i != j ) {
        eduI[j] = probEduChlg(i, i, i, j, recordTmpSQLP);
    }
  }

  return eduI;
}

// --------------------------------------------
vector<double> SMPState::calcVotes(KMatrix w, KMatrix u, int k) const
{
	auto vfn = [&w, &u](VotingRule vr, unsigned int k, unsigned int i, unsigned int j) {
		double vkij = Model::vote(vr, w(0, k), u(k, i), u(k, j));
		return vkij;
	};
	auto li = u.numC();  
	auto lj = w.numC();
	vector<double> votes = {};
	for (unsigned int i = 0; i < li; i++) //li is the number of bargains
	{
		for (unsigned int j = 0; j < i; j++)
		{
			auto vr = ((const SMPActor*)(model->actrs[k]))->vr;
			double vkij = vfn(vr, k, i, j);
			votes.push_back(vkij);
			
		}
	}
	return votes;
}

SMPState* SMPState::doBCN() {
  const unsigned int na = model->numAct;
  brgns.resize(na);
  for (unsigned int i = 0; i < na; i++) {
    brgns[i] = vector<BargainSMP*>();
  }

  auto thrBCN = [this](unsigned int i) {
    this->doBCN(i);
  };

  KBase::groupThreads(thrBCN, 0, na - 1);

  model->beginDBTransaction();

  if (model->sqlFlags[2]) {
    recordProbEduChlg();
  }

  if (model->sqlFlags[3]) {
    for (auto brgnCoord : brgnCos) {
      model->sqlBargainCoords(
        get<0>(brgnCoord), //turn
        get<1>(brgnCoord), //bargnId
        get<2>(brgnCoord), //posInit
        get<3>(brgnCoord)  //posRcvr
      );
    }
  }

  if (model->sqlFlags[4]) {
    for (auto brgnVal : brgnVals) {
      model->sqlBargainEntries(
        get<0>(brgnVal), //turn
        get<1>(brgnVal), //bargnId
        get<2>(brgnVal), //initiator
        get<3>(brgnVal), //receiver
        get<4>(brgnVal)  //value
      );
    }
  }

  //model->commitDBTransaction();

  LOG(INFO) << "Bargains to be resolved";
  showBargains(brgns);

  w = actrCaps();
  LOG(INFO) << "w:";
  w.mPrintf(" %6.2f ");

  s2 = new SMPState(model);

  auto thrCalcPosts = [this](unsigned int k) {
    this->updateBestBrgnPositions(k);
  };

  KBase::groupThreads(thrCalcPosts, 0, na - 1);

  //model->beginDBTransaction();

  if (model->sqlFlags[3]) {
    for (auto votes : brgnVotes) {
      for (auto vote : votes) {
        model->sqlBargainVote(
          get<0>(vote), //turn
          get<1>(vote), //barginIDsPair_i_j
          get<2>(vote), //pv_ij
          get<3>(vote)  //actor
        );
      }
    }

    for (auto util : brgnUtils) {
      model->sqlBargainUtil(
        get<0>(util), //turn
        get<1>(util), //bargnIds
        get<2>(util)  //utilities
      );
    }
  }

  // record data so far
  if (model->sqlFlags[4]) {
    updateBargnTable(brgns, actorBargains, actorMaxBrgNdx);
  }

  model->commitDBTransaction();

  // Some bargains are nullptr, and there are two copies of every non-nullptr randomly
  // arranged. If we delete them as we find them, then the second occurance will be corrupted,
  // so the code crashes when it tries to access the memory to see if it matches something
  // already deleted. Hence, we scan them all, building a list of unique bargains,
  // then delete those in order.
  auto uBrgns = vector<BargainSMP*>();

  for (unsigned int i = 0; i < brgns.size(); i++) {
    auto ai = ((const SMPActor*)(model->actrs[i]));
    for (unsigned int j = 0; j < brgns[i].size(); j++) {
      BargainSMP* bij = brgns[i][j];
      if (nullptr != bij) {
        if (ai == bij->actInit) {
          uBrgns.push_back(bij); // this is the initiator's copy, so save it for deletion
        }
      }
      brgns[i][j] = nullptr; // either way, null it out.
    }
  }

  for (auto b : uBrgns) {
    //int aI = model->actrNdx(b->actInit);
    //int aR = model->actrNdx(b->actRcvr);
    //printf("Delete bargain [%2i:%2i] \n", aI, aR);
    delete b;
  }

  // TODO: this really should do all the assessment: ueIndices, rnProb, all U^h_{ij}, raProb
  s2->setUENdx();

  if (0 == accomodate.numC()) { // nothing to copy
    s2->setAccomodate(1.0); // set to identity matrix
  }
  else {
    s2->setAccomodate(accomodate);
  }

  if (0 == ideals.size()) { // nothing to copy
    s2->idealsFromPstns(); // set s2's current ideals to s2's current positions
  }
  else {
    s2->ideals = ideals; // copy s1's old ideals
  }
  s2->newIdeals(); // adjust s2 ideals toward new ones
  double ipDist = s2->posIdealDist(ReportingLevel::Medium);
  LOG(INFO) << KBase::getFormattedString("rms (pstn, ideal) = %.5f", ipDist);
  return s2;
}

void SMPState::doBCN(unsigned int i) {
    auto ai = ((const SMPActor*)(model->actrs[i]));
    auto posI = ((const VctrPstn*)pstns[i]);
    auto smod = dynamic_cast<SMPModel *>(model);
    const InterVecBrgn ivb = smod->ivBrgn;
    const SMPBargnModel bMod = smod->brgnMod;

    auto sqBrgnI = new BargainSMP(ai, ai, *posI, *posI);
    brgnsLock.lock();
    brgns[i].push_back(sqBrgnI);
    brgnsLock.unlock();

    // before we can log this bargain, we need to get the group ID for this table
    // so then we can get the flag to populate the table or not
    // note the implicit assumption that there will never be 43+ groups :-)
    unsigned int grpID = 42;
    for (unsigned int t = 0; t<model->KTables.size(); t++)
    {
        if (model->KTables[t]->tabName == "Bargn")
        {
            grpID = model->KTables[t]->tabGrpID;
            break;
        }
    }
    // be sure that it found this table
    assert(grpID != 42);
    assert(grpID < model->sqlFlags.size());

    if (model->sqlFlags[grpID])
    {
      brgnValsLock.lock();
      brgnVals.push_back(BrgnValue(turn, sqBrgnI->getID(), i, i, 0));
      brgnValsLock.unlock();
    }

    eduChlgsI eduI = bestChallengeUtils(i);

    auto chlgI = bestChallenge(eduI);
    const double bestEU = get<2>(chlgI);
    if (0 < bestEU) {
      unsigned int bestJ = get<0>(chlgI); //
      const double piiJ = get<1>(chlgI); // i's estimate of probability i defeats j
      assert(0 <= bestJ);
      const unsigned int j = bestJ; // for consistency in code below

      auto aj = ((const SMPActor*)(model->actrs[j]));
      auto posJ = ((const VctrPstn*)pstns[j]);

      std::thread thr(&SMPState::calcUtils, this, i, bestJ);

      // make the variables local to lexical scope of this block.
      // for testing, calculate and print out a block of data showing each's perspective
      bool recordTmpSQLP = true;  // Record this in SQLite
      auto pFn = [this, recordTmpSQLP](unsigned int h, unsigned int k, unsigned int i, unsigned int j) {
        auto est = probEduChlg(h, k, i, j, recordTmpSQLP); // H's estimate of the effect on K of I->J
        double phij = get<0>(est);
        double edu_hk_ij = get<1>(est);
        return est;
      };

      auto est_ijij = pFn(i, j, i, j); // I's estimate of the effect on J of I->J

      auto Vjij = pFn(j, i, i, j); // J's estimate of the effect on I of I->J

      auto est_jjij = pFn(j, j, i, j); // J's estimate of the effect on J of I->J

      // interpolate a bargain from I's perspective
      BargainSMP* brgnIIJ = SMPActor::interpolateBrgn(ai, aj, posI, posJ, piiJ, 1 - piiJ, ivb);
      const int nai = model->actrNdx(brgnIIJ->actInit);
      const int naj = model->actrNdx(brgnIIJ->actRcvr);
      // verify that identities match up as expected
      assert(nai == i);
      assert(naj == j);

      // interpolate a bargain from targeted J's perspective
      double pjiJ = get<1>(Vjij); // j's estimate of the probability that i defeats j
      BargainSMP* brgnJIJ = SMPActor::interpolateBrgn(ai, aj, posI, posJ, pjiJ, 1 - pjiJ, ivb);

      // calcluate weights as capability times salience
      double sci = brgnIIJ->actInit->sCap;
      double svi = sum(brgnIIJ->actInit->vSal);
      double wi = sci*svi;
      double scj = brgnJIJ->actInit->sCap;
      double svj = sum(brgnIIJ->actRcvr->vSal);
      double wj = scj*svj;

      // create a new bargain whose positions are the weighted averages
      auto bpi = VctrPstn((wi*brgnIIJ->posInit + wj*brgnJIJ->posInit) / (wi + wj));
      auto bpj = VctrPstn((wi*brgnIIJ->posRcvr + wj*brgnJIJ->posRcvr) / (wi + wj));
      BargainSMP *brgnIJ = new  BargainSMP(brgnIIJ->actInit, brgnIIJ->actRcvr, bpi, bpj);

      mtxLock.lock();
      LOG(INFO) << KBase::getFormattedString(
        "In turn %i actor %u has most advantageous target %u worth %.3f",
        turn, i, j, bestEU);

      // Look for counter-intuitive cases
      if (piiJ < 0.5) {
        LOG(INFO) << "turn" << turn << ","
            << "i" << i << ","
            << "j" << j << ","
            << "bestEU worth" << bestEU << ","
            << "piiJ " << piiJ;
      }

      // I's estimate of the effect on I of I->J
      LOG(INFO) << KBase::getFormattedString(
        "Est by %2u of prob %.4f that [%2u>%2u], with expected gain to %2u of %+.4f",
        i, piiJ, i, j, i, get<2>(chlgI));

      // I's estimate of the effect on J of I->J
      LOG(INFO) << KBase::getFormattedString(
          "Est by %2u of prob %.4f that [%2u>%2u], with expected gain to %2u of %+.4f",
          i, get<0>(est_ijij), i, j, j, get<1>(est_ijij));

      // J's estimate of the effect on I of I->J
      LOG(INFO) << KBase::getFormattedString(
          "Est by %2u of prob %.4f that [%2u>%2u], with expected gain to %2u of %+.4f",
          j, get<0>(Vjij), i, j, i, get<1>(Vjij));

      // J's estimate of the effect on J of I->J
      LOG(INFO) << KBase::getFormattedString(
          "Est by %2u of prob %.4f that [%2u>%2u], with expected gain to %2u of %+.4f",
          j, get<0>(est_jjij), i, j, j, get<1>(est_jjij));
      LOG(INFO) << "";

      // Bargain positions from i's perspective
      LOG(INFO) << "Bargain" << showOneBargain(brgnIIJ)
        << "from" << std::to_string(i) + "'s perspective (brgnIIJ)";
      //LOG(INFO) << i << "proposes" << i << "adopt:";
      string proposal = string("   ") + std::to_string(i) + " proposes " + std::to_string(i) + " adopt: ";
      (KBase::trans(brgnIIJ->posInit) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]
      //LOG(INFO) << i << "proposes" << j << "adopt:";
      proposal = string("   ") + std::to_string(i) + " proposes " + std::to_string(j) + " adopt: ";
      (KBase::trans(brgnIIJ->posRcvr) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]
      LOG(INFO) << "";

      // Bargain positions from j's perspective
      LOG(INFO) << "Bargain" << showOneBargain(brgnJIJ)
        << "from" << std::to_string(j) + "'s perspective (brgnIIJ)";
      //LOG(INFO) << j << "proposes" << i << "adopt:";
      proposal = string("   ") + std::to_string(j) + " proposes " + std::to_string(i) + " adopt: ";
      (KBase::trans(brgnJIJ->posInit) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]
      //LOG(INFO) << j << "proposes" << j << "adopt:";
      proposal = string("   ") + std::to_string(j) + " proposes " + std::to_string(j) + " adopt: ";
      (KBase::trans(brgnJIJ->posRcvr) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]
      LOG(INFO) << "";

      // Power-weighted compromise
      LOG(INFO) << "Power-weighted compromise" << showOneBargain(brgnIJ) << "bargain (brgnIJ)";
      //LOG(INFO) << "  Compromise proposes" << i << "adopt: ";
      proposal = string("   ") + string("  compromise proposes ") + std::to_string(i) + " adopt: ";
      (KBase::trans(brgnIJ->posInit) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]

      //LOG(INFO) << "  Compromise proposes" << j << "adopt: ";
      proposal = string("   ") + string("  compromise proposes ") + std::to_string(j) + " adopt: ";
      (KBase::trans(brgnIJ->posRcvr) * 100.0).mPrintf(" %.3f ", proposal); // print on the scale of [0,100]
      LOG(INFO) << "";


      // TODO: make one-perspective an option.
      // For now, emulate it by swapping
      //auto tIJ = brgnIJ;
      //auto tIIJ = brgnIIJ;
      //brgnIJ = tIIJ;
      //brgnIIJ = tIJ;

      LOG(INFO) << "Using" << bMod << "to form proposed bargains";
      mtxLock.unlock();
      switch (bMod) {
      case SMPBargnModel::InitOnlyInterpSMPBM:
        // record the only one used into SQLite JAH 20160802 use the flag
        if(model->sqlFlags[grpID])
        {
          brgnValsLock.lock();
          brgnVals.push_back(BrgnValue(turn, brgnIIJ->getID(), i, j, bestEU));
          brgnValsLock.unlock();
        }
        if(model->sqlFlags[3])
        {          
          brgnCosLock.lock();
          brgnCos.push_back(BrgnCoord(turn, brgnIIJ->getID(), brgnIIJ->posInit, brgnIIJ->posRcvr));
          brgnCosLock.unlock();
        }
        // record this one onto BOTH the initiator and receiver queues
        brgnsLock.lock();
        brgns[i].push_back(brgnIIJ); // initiator's copy, delete only it later
        brgns[j].push_back(brgnIIJ); // receiver's copy, just null it out later
        brgnsLock.unlock();
        // clean up unused
        delete brgnIJ;
        brgnIJ = nullptr;
        delete brgnJIJ;
        brgnJIJ = nullptr;
        break;


      case SMPBargnModel::InitRcvrInterpSMPBM:
        // record the pair used into SQLite JAH 20160802 use the flag
        if(model->sqlFlags[grpID])
        {
          brgnValsLock.lock();
          brgnVals.push_back(BrgnValue(turn, brgnIIJ->getID(), i, j, bestEU));
          brgnVals.push_back(BrgnValue(turn, brgnJIJ->getID(), i, j, bestEU));
          brgnValsLock.unlock();
        }
        if(model->sqlFlags[3])
        {
          brgnCosLock.lock();
          brgnCos.push_back(BrgnCoord(turn, brgnIIJ->getID(), brgnIIJ->posInit, brgnIIJ->posRcvr));
          brgnCos.push_back(BrgnCoord(turn, brgnJIJ->getID(), brgnJIJ->posInit, brgnJIJ->posRcvr));
          brgnCosLock.unlock();
        }
        // record these both onto BOTH the initiator and receiver queues
        brgnsLock.lock();
        brgns[i].push_back(brgnIIJ); // initiator's copy, delete only it later
        brgns[i].push_back(brgnJIJ); // initiator's copy, delete only it later
        brgns[j].push_back(brgnIIJ); // receiver's copy, just null it out later
        brgns[j].push_back(brgnJIJ); // receiver's copy, just null it out later
        brgnsLock.unlock();
        // clean up unused
        delete brgnIJ;
        brgnIJ = nullptr;
        break;


      case SMPBargnModel::PWCompInterpSMPBM:
        // record the only one used into SQLite JAH 20160802 use the flag
        if(model->sqlFlags[grpID])
        {
          brgnValsLock.lock();
          brgnVals.push_back(BrgnValue(turn, brgnIJ->getID(), i, j, bestEU));
          brgnValsLock.unlock();
        }
        if(model->sqlFlags[3])
        {
          brgnCosLock.lock();
          brgnCos.push_back(BrgnCoord(turn, brgnIJ->getID(), brgnIJ->posInit, brgnIJ->posRcvr));
          brgnCosLock.unlock();
        }
        // record this one onto BOTH the initiator and receiver queues
        brgnsLock.lock();
        brgns[i].push_back(brgnIJ); // initiator's copy, delete only it later
        brgns[j].push_back(brgnIJ); // receiver's copy, just null it out later
        brgnsLock.unlock();
        // clean up unused
        delete brgnIIJ;
        brgnIIJ = nullptr;
        delete brgnJIJ;
        brgnJIJ = nullptr;
        break;

      default:
        LOG(INFO) << "SMPState::doBCN unrecognized SMPBargnModel";
        exit(-1);
      }

      thr.join();
    }
    else {
      LOG(INFO) << "In turn" << turn << "Actor" << i << "has no advantageous targets";
    }
}

void SMPState::updateBestBrgnPositions(int k) {
  auto ndxMaxProb = [](const KMatrix & cv) {
    const double pTol = 1E-8;
    assert(fabs(KBase::sum(cv) - 1.0) < pTol);
    assert(0 < cv.numR());
    assert(1 == cv.numC());
    auto ndxIJ = ndxMaxAbs(cv);
    unsigned int iMax = get<0>(ndxIJ);
    return iMax;
  };

  // what is the utility to actor nai of the state resulting after
  // the nbj-th bargain of the k-th actor is implemented?
  auto brgnUtil = [this](unsigned int nk, unsigned int nai, unsigned int nbj) {
    const unsigned int na = model->numAct;
    BargainSMP * b = brgns[nk][nbj];
    assert(nullptr != b);
    double uAvrg = 0.0;

    if (b->actInit == b->actRcvr) { // SQ bargain
      uAvrg = 0.0;
      for (unsigned int n = 0; n < na; n++) {
        // nai's estimate of the utility to nai of position n, i.e. the true value
        uAvrg = uAvrg + aUtil[nai](nai, n);
      }
    }

    else { // all positions unchanged, except Init and Rcvr
      uAvrg = 0.0;
      auto ndxInit = model->actrNdx(b->actInit);
      assert((0 <= ndxInit) && (ndxInit < na)); // must find it
      double uPosInit = ((SMPActor*)(model->actrs[nai]))->posUtil(&(b->posInit), this);
      uAvrg = uAvrg + uPosInit;

      auto ndxRcvr = model->actrNdx(b->actRcvr);
      assert((0 <= ndxRcvr) && (ndxRcvr < na)); // must find it
      double uPosRcvr = ((SMPActor*)(model->actrs[nai]))->posUtil(&(b->posRcvr), this);
      uAvrg = uAvrg + uPosRcvr;

      for (unsigned int n = 0; n < na; n++) {
        if ((ndxInit != n) && (ndxRcvr != n)) {
          // again, nai's estimate of the utility to nai of position n, i.e. the true value
          uAvrg = uAvrg + aUtil[nai](nai, n);
        }
      }
    }

    uAvrg = uAvrg / na;

    assert(0.0 < uAvrg); // none negative, at least own is positive
    assert(uAvrg <= 1.0); // can not all be over 1.0
    return uAvrg;
  };
  // end of λ-fn


  // The key is to build the usual matrix of U_ai (Brgn_m) for all bargains in brgns[k],
  // making sure to divide the sum of the utilities of positions by 1/N
  // so 0 <= Util(state after Brgn_m) <= 1, then do the standard scalarPCE for bargains involving k.

    auto buk = [brgnUtil, k](unsigned int nai, unsigned int nbj) {
      return brgnUtil(k, nai, nbj);
    };
    auto smod = dynamic_cast<SMPModel *>(model);
    unsigned int na = smod->numAct;
    unsigned int nb = brgns[k].size();

    mtxLock.lock();
    auto u_im = KMatrix::map(buk, na, nb);

    LOG(INFO) << "u_im:";
    u_im.mPrintf(" %.5f ");

    LOG(INFO) << "Doing scalarPCE for the" << nb << "bargains of actor" << k << "...";
    auto p = Model::scalarPCE(na, nb, w, u_im, smod->vrCltn, smod->vpm, smod->pcem, ReportingLevel::Medium);
    assert(nb == p.numR());
    assert(1 == p.numC());
    actorBargains.insert(map<unsigned int, KBase::KMatrix>::value_type(k, p));

    unsigned int mMax = nb; // indexing actors by i, bargains by m
    switch (smod->stm) {
    case StateTransMode::DeterminsticSTM:
      mMax = ndxMaxProb(p);
      break;
    case StateTransMode::StochasticSTM:
      mMax = model->rng->probSel(p);
      break;
    default:
      throw KException("SMPState::doBCN - unrecognized StateTransMode");
      break;
    }
    // 0 <= mMax assured for uint
    assert(mMax < nb);
    actorMaxBrgNdx.insert(map<unsigned int, unsigned int>::value_type(k, mMax));
    auto bkm = brgns[k][mMax];
    LOG(INFO) << "Chosen bargain (" << smod->stm << "):" << bkm->getID()
      << mMax + 1 << "out of" << nb << "bargains";
    mtxLock.unlock();

    //populate the Bargain Vote & Util tables
    // JAH added sql flag logging control
  if (model->sqlFlags[3])
  {
    auto brgns_k = brgns[k];
    vector< std::tuple<uint64_t, uint64_t>> barginIDsPair_i_j;
    vector<uint64_t> bargnIdsRows = {};
    for (int j = 0; j < nb; j++)
    {
      bargnIdsRows.push_back(brgns[k][j]->getID());
    }
    for (unsigned int brgnFirst = 0; brgnFirst < nb; brgnFirst++)
    {
      for (unsigned int brgnSecond = 0; brgnSecond < brgnFirst; brgnSecond++)
      {
        barginIDsPair_i_j.push_back(tuple<uint64_t, uint64_t>(brgns_k[brgnFirst]->getID(), brgns_k[brgnSecond]->getID()));
      }
    }

    BrgnVotes votes;
    for (unsigned int actor = 0; actor < na; ++actor) {
      auto pv_ij = calcVotes(w, u_im, actor);

      votes.push_back(BrgnVote(turn, barginIDsPair_i_j, pv_ij, actor));
    }
    brgnPosLock.lock();
    brgnVotes.push_back(votes);
    brgnUtils.push_back(BrgnUtil(turn, bargnIdsRows, u_im));
    brgnPosLock.unlock();
  }

    // TODO: create a fresh position for k, from the selected bargain mMax.
    VctrPstn * pk = nullptr;
    auto oldPK = dynamic_cast<VctrPstn *>(pstns[k]);
    if (bkm->actInit == bkm->actRcvr) { // SQ
      pk = new VctrPstn(*oldPK);
    }
    else {
      const unsigned int ndxInit = model->actrNdx(bkm->actInit);
      const unsigned int ndxRcvr = model->actrNdx(bkm->actRcvr);
      if (ndxInit == k) {
        pk = new VctrPstn(bkm->posInit);
      }
      else if (ndxRcvr == k) {
        pk = new VctrPstn(bkm->posRcvr);
      }
      else {
        LOG(INFO) << "SMPState::doBCN: unrecognized actor in bargain";
        assert(false);
        exit(-1);
      }

      // If the actor has changed its position, record the bargain id
      for (int dimen = 0; dimen < pk->numR(); dimen++) {
        auto pCoordOld = (*oldPK)(dimen, 0);
        auto pCoord = (*pk)(dimen, 0);
        if (pCoord != pCoordOld) {
          s2->setPosMoverBargain(k, bkm->getID());
        }
      }
    }
    assert(nullptr != pk);

    // Make sure that the pk is stored at right position in s2.
    s2->pstns[k] = pk;
}


// h's estimate of the victory probability and expected delta in utility for k from i challenging j,
// compared to status quo.
// Note that the  aUtil vector of KMatrix must be set before starting this.
// TODO: offer a choice the different ways of estimating value-of-a-state: even sum or expected value.
// TODO: we may need to separate euConflict from this at some point
tuple<double, double> SMPState::probEduChlg(unsigned int h, unsigned int k, unsigned int i, unsigned int j, bool sqlP) const {

  // you could make other choices for these two sub-models
  auto sMod = (const SMPModel*)model;
  auto vr = sMod->vrCltn; //VotingRule::Proportional;
  auto tpc = sMod->tpCommit;// KBase::ThirdPartyCommit::SemiCommit;

  double uii = aUtil[h](i, i);
  double uij = aUtil[h](i, j);
  double uji = aUtil[h](j, i);
  double ujj = aUtil[h](j, j);

  // h's estimate of utility to k of status-quo positions of i and j
  const double euSQ = aUtil[h](k, i) + aUtil[h](k, j);
  assert((0.0 <= euSQ) && (euSQ <= 2.0));

  // h's estimate of utility to k of i defeating j, so j adopts i's position
  const double uhkij = aUtil[h](k, i) + aUtil[h](k, i);
  assert((0.0 <= uhkij) && (uhkij <= 2.0));

  // h's estimate of utility to k of j defeating i, so i adopts j's position
  const double uhkji = aUtil[h](k, j) + aUtil[h](k, j);
  assert((0.0 <= uhkji) && (uhkji <= 2.0));

  auto ai = ((const SMPActor*)(model->actrs[i]));
  double si = KBase::sum(ai->vSal);
  double ci = ai->sCap;
  auto aj = ((const SMPActor*)(model->actrs[j]));
  double sj = KBase::sum(aj->vSal);
  assert((0 < sj) && (sj <= 1));
  double cj = aj->sCap;
  const double minCltn = 1E-10;

  // get h's estimate of the principal actors' contribution to their own contest

  // h's estimate of i's unilateral influence contribution to (i:j).
  // When ideals perfectly track positions, this must be positive
  double contrib_i_ij = Model::vote(vr, si*ci, uii, uij);
  if (identAccMat) {
    assert(0 <= contrib_i_ij);
  }
  // If not, you could have the ordering (Idl_i, Pos_j, Pos_i)
  // so that i would prefer j's position over his own.


  // h's estimate of j's unilateral influence contribution to (i:j).
  // When ideals perfectly track positions, this must be negative
  double contrib_j_ij = Model::vote(vr, sj*cj, uji, ujj);
  if (identAccMat) {
    assert(contrib_j_ij <= 0);
  }
  // Similarly, you could have an ordering like (Idl_j, Pos_i, Pos_j)
  // so that j would prefer i's position over his own.

  double chij = minCltn; // strength of complete coalition supporting i over j (initially empty)
  double chji = minCltn; // strength of complete coalition supporting j over i (initially empty)

  // add i's contribution to the appropriate coalition
  if (contrib_i_ij > 0.0) {
    chij = chij + contrib_i_ij;
  }
  assert(0.0 < chij);

  if (contrib_i_ij < 0.0) {
    chji = chji - contrib_i_ij;
  }
  assert(0.0 < chji);

  // add j's contribution to the appropriate coalition
  if (contrib_j_ij > 0.0) {
    chij = chij + contrib_j_ij;
  }
  assert(0.0 < chij);

  if (contrib_j_ij < 0.0) {
    chji = chji - contrib_j_ij;
  }
  assert(0.0 < chji);

  // cache those sums
  contrib_i_ij = chij;
  contrib_j_ij = chji;


  const unsigned int na = model->numAct;

  // we assess the overall coalition strengths by adding up the contribution of
  // individual actors (including i and j, above). We assess the contribution of third
  // parties (n) by looking at little coalitions in the hypothetical (in:j) or (i:nj) contests.
  auto tpvArray = KMatrix(na, 3);
  for (unsigned int n = 0; n < na; n++) {
    if ((n != i) && (n != j)) { // already got their influence-contributions
      auto an = ((const SMPActor*)(model->actrs[n]));

      double cn = an->sCap;
      double sn = KBase::sum(an->vSal);
      double uni = aUtil[h](n, i);
      double unj = aUtil[h](n, j);
      double unn = aUtil[h](n, n);

      // notice that each third party starts afresh,
      // considering only contributions of principals and itself
      double pin = Actor::vProbLittle(vr, sn*cn, uni, unj, contrib_i_ij, contrib_j_ij);

      assert(0.0 <= pin);
      assert(pin <= 1.0);
      double pjn = 1.0 - pin;
      auto vt_uv_ul = Actor::thirdPartyVoteSU(sn*cn, vr, tpc, pin, pjn, uni, unj, unn);
      const double vnij = get<0>(vt_uv_ul);
      chij = (vnij > 0) ? (chij + vnij) : chij;
      assert(0 < chij);
      chji = (vnij < 0) ? (chji - vnij) : chji;
      assert(0 < chji);

      const double utpv = get<1>(vt_uv_ul);
      const double utpl = get<2>(vt_uv_ul);
      // record for SQLite
      tpvArray(n, 0) = pin;
      tpvArray(n, 1) = utpv;
      tpvArray(n, 2) = utpl;
    }
  }

  const double phij = chij / (chij + chji); // ProbVict, for i
  const double phji = chji / (chij + chji);

  const double euVict = uhkij;  // UtilVict
  const double euCntst = phij*uhkij + phji*uhkji; // UtilContest,
  const double euChlg = (1 - sj)*euVict + sj*euCntst; // UtilChlg
  const double duChlg = euChlg - euSQ; //  delta-util of challenge versus status-quo
  auto rslt = tuple<double, double>(phij, duChlg);

  // JAH 20160802 switched to use the model sql flags vector to control logging
  // I keep sqlP and short-circuit & it because sometimes probEduChlg is called to
  // do some temporary calcs which should not be store - this is controlled with sqlP
  if (sqlP && model->sqlFlags[2]) {
    // now that the computation is finished, record everything into SQLite
    //
    // record tpvArray into SQLite turn, est (h), init (i), third party (n), receiver (j), and tpvArray[n]
    // printf ("SMPState::probEduChlg(%2i, %2i, %2i, %i2) = %+6.4f - %+6.4f = %+6.4f\n", h, k, i, j, euCh, euSQ, euChlg);

    string thij = std::to_string(turn)
      + "," + std::to_string(h)
      + "," + std::to_string(i)
      + "," + std::to_string(j);

    // formatting note: %d means an integer, base 10
    // we will use base 10 by default, and these happen to be unsigned integers, so %i is appropriate

    string thkij = std::to_string(turn)
      + "," + std::to_string(h)
      + "," + std::to_string(k)
      + "," + std::to_string(i)
      + "," + std::to_string(j);

    std::vector<double> eu;
    eu.push_back(euSQ);
    eu.push_back(euVict);
    eu.push_back(euCntst);
    eu.push_back(euChlg);

    // Thread safety lock
    utilDataLock.lock();
    euData.emplace(thkij,eu);
    tpvData.emplace(thij, tpvArray);
    phijData.emplace(thij, phij);
    utilDataLock.unlock();
  }
  return rslt;
}


tuple<int, double, double> SMPState::bestChallenge(eduChlgsI &eduI) const {
  int bestJ = -1;
  double pIJ = 0;
  double bestEU = -1.00;

  // for SMP, positive expected gains on the first turn are typically in the 0.5 to 0.01 range
  // I take a fraction of the minimum.
  const double minSigEDU = 1e-5; // TODO: 1/20 of the minimum, or 0.0005

  for(const auto& eduIJ : eduI) {
    double pij = get<0>(eduIJ.second);
    double edu = get<1>(eduIJ.second);
    if ((minSigEDU < edu) && (bestEU < edu)) {
      bestJ = eduIJ.first;
      pIJ = pij;
      bestEU = edu;
    }
  }
  if (0 <= bestJ) {
    assert(minSigEDU < bestEU);
  }
  auto rslt = tuple<int, double, double>(bestJ, pIJ, bestEU);
  return rslt;
}

uint64_t SMPState::getPosMoverBargain(unsigned int actor) const {
  return positionMovers.at(actor);
}

void SMPState::setPosMoverBargain(unsigned int actor, uint64_t bargainID) {
  positionMovers.insert(moverBargains::value_type(actor, bargainID));
}

}; // end of namespace

// --------------------------------------------
// Copyright KAPSARC. Open source MIT License.
// --------------------------------------------
