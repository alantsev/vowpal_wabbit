#include <float.h>
#include "reductions.h"
#include "cb_algs.h"
#include "rand48.h"
#include "bs.h"
#include "../explore/static/MWTExplorer.h"
#include "vw.h"

using namespace LEARNER;
using namespace MultiWorldTesting;

struct vw_context;
void safety(v_array<float>& distribution, float min_prob);

class vw_policy : public IPolicy<vw_context>
{
public:
  vw_policy() : m_index(-1) { }
  vw_policy(size_t i) : m_index((int)i) { }
  u32 Choose_Action(vw_context& ctx);
private:
  int m_index;
};

class vw_cover_scorer : public IScorer<vw_context>
{
public:
  vw_cover_scorer(float epsilon, size_t cover, u32 num_actions) :
    m_epsilon(epsilon), m_cover(cover), m_num_actions(num_actions), m_counter(1)
  { 
    m_scores = v_init<float>();
    m_predictions = v_init<uint32_t>();
    m_scores.resize(num_actions + 1);
    m_predictions.resize(m_cover);
  }
  
  float Get_Epsilon() { return m_epsilon; }
  size_t Get_Cover() { return m_cover; }
  size_t Get_Counter() { return m_counter; }
  v_array<uint32_t>& Get_Predictions() { return m_predictions; };
  v_array<float>& Get_Scores()
  { 
    m_scores.erase();
    for (size_t i = 0; i < m_num_actions; i++)
      {
        m_scores.push_back(0);
      }
    return m_scores; 
  };
  
  vector<float> Score_Actions(vw_context& ctx);
  
private:
  float m_epsilon;
  size_t m_cover;
  u32 m_num_actions;
  size_t m_counter;
  v_array<float> m_scores;
  v_array<uint32_t> m_predictions;
};

class vw_recorder : public IRecorder<vw_context>
{
public:
  void Record(vw_context& context, u32 action, float probability, string unique_key);
  u32 Get_Action() { return m_action; }
  float Get_Prob() { return m_prob; }
private:
  u32 m_action;
  float m_prob;
};

struct cbify {
  
  size_t k;
  
  CB::label cb_label;
  COST_SENSITIVE::label cs_label;
  COST_SENSITIVE::label second_cs_label;
  
  base_learner* cs;
  vw* all;
  
  unique_ptr<vw_policy> policy;
  vector<unique_ptr<IPolicy<vw_context>>> policies;
  unique_ptr<vw_cover_scorer> scorer;
  unique_ptr<vw_recorder> recorder;
  unique_ptr<MwtExplorer<vw_context>> mwt_explorer;
  unique_ptr<TauFirstExplorer<vw_context>> tau_explorer;
  unique_ptr<EpsilonGreedyExplorer<vw_context>> greedy_explorer;
  unique_ptr<BootstrapExplorer<vw_context>> bootstrap_explorer;
  unique_ptr<GenericExplorer<vw_context>> generic_explorer;
};

float loss(uint32_t label, uint32_t final_prediction)
{
  if (label != final_prediction)
    return 1.;
  else
    return 0.;
}

struct vw_context {
  base_learner* l;
  example* e;
  cbify* data;
  bool recorded;
};

u32 vw_policy::Choose_Action(vw_context& ctx)
{
  if (m_index == -1)
    ctx.l->predict(*ctx.e);
  else
    ctx.l->predict(*ctx.e, (size_t)m_index);
  ctx.recorded = false;
  return (u32)(ctx.e->pred.multiclass);
}

void vw_recorder::Record(vw_context& context, u32 action, float probability, string unique_key)
{
  m_action = action;
  m_prob = probability;
  context.recorded = true;
}

vector<float> vw_cover_scorer::Score_Actions(vw_context& ctx)
{
  float additive_probability = 1.f / (float)m_cover;
  for (size_t i = 0; i < m_cover; i++)
    { //get predicted cost-sensitive predictions
      if (i == 0)
        ctx.data->cs->predict(*ctx.e, i);
      else
        ctx.data->cs->predict(*ctx.e, i + 1);
      uint32_t pred = ctx.e->pred.multiclass;
      m_scores[pred - 1] += additive_probability;
      m_predictions[i] = (uint32_t)pred;
    }
  float min_prob = m_epsilon * min(1.f / ctx.data->k, 1.f / (float)sqrt(m_counter * ctx.data->k));
  
  safety(m_scores, min_prob);
  
  vector<float> scores;
  for (size_t i = 0; i < ctx.data->k; i++)
    scores.push_back(m_scores[i]);
  
  m_counter++;
  
  return scores;
}

template <bool is_learn>
void predict_or_learn_first(cbify& data, base_learner& base, example& ec)
{//Explore tau times, then act according to optimal.
  MULTICLASS::label_t ld = ec.l.multi;
  
  data.cb_label.costs.erase();
  ec.l.cb = data.cb_label;
  //Use CB to find current prediction for remaining rounds.
  
  vw_context vwc;
  vwc.l = &base;
  vwc.e = &ec;
  
  uint32_t action = data.mwt_explorer->Choose_Action(*data.tau_explorer.get(), to_string((unsigned long long)ec.example_counter), vwc);
  ec.loss = loss(ld.label, action);
  
  if (vwc.recorded && is_learn)
    {
      CB::cb_class l = {ec.loss, action, 1.f / data.k, 0};
      ec.l.cb.costs.push_back(l);
      base.learn(ec);
      ec.loss = l.cost;
    }
  
  ec.pred.multiclass = action;
  ec.l.multi = ld;
}

template <bool is_learn>
void predict_or_learn_greedy(cbify& data, base_learner& base, example& ec)
{//Explore uniform random an epsilon fraction of the time.
  MULTICLASS::label_t ld = ec.l.multi;
  
  data.cb_label.costs.erase();
  ec.l.cb = data.cb_label;
  
  vw_context vwc;
  vwc.l = &base;
  vwc.e = &ec;
  data.mwt_explorer->Choose_Action(*data.greedy_explorer.get(), to_string((unsigned long long)ec.example_counter), vwc);
  
  u32 action = data.recorder->Get_Action();
  float prob = data.recorder->Get_Prob();
  
  CB::cb_class l = { loss(ld.label, action), action, prob };
  ec.l.cb.costs.push_back(l);
  
  if (is_learn)
    base.learn(ec);
  
  ec.pred.multiclass = action;
  ec.l.multi = ld;
  ec.loss = loss(ld.label, action);
}

template <bool is_learn>
void predict_or_learn_bag(cbify& data, base_learner& base, example& ec)
{//Randomize over predictions from a base set of predictors
  //Use CB to find current predictions.
  MULTICLASS::label_t ld = ec.l.multi;
  
  data.cb_label.costs.erase();
  ec.l.cb = data.cb_label;
  
  vw_context context;
  context.l = &base;
  context.e = &ec;
  uint32_t action = data.mwt_explorer->Choose_Action(*data.bootstrap_explorer.get(), to_string((unsigned long long)ec.example_counter), context);
  
  assert(action != 0);
  if (is_learn)
    {
      assert(action == data.recorder->Get_Action());
      float probability = data.recorder->Get_Prob();
      
      CB::cb_class l = {loss(ld.label, action), 
			action, probability};
      ec.l.cb.costs.push_back(l);
      for (size_t i = 0; i < data.policies.size(); i++)
	{
	  uint32_t count = BS::weight_gen();
	  for (uint32_t j = 0; j < count; j++)
	    base.learn(ec,i);
	}
    }
  ec.pred.multiclass = action;
  ec.l.multi = ld;
}
  
  void safety(v_array<float>& distribution, float min_prob)
  {
    float added_mass = 0.;
    for (uint32_t i = 0; i < distribution.size();i++)
      if (distribution[i] > 0 && distribution[i] <= min_prob)
	{
	  added_mass += min_prob - distribution[i];
	  distribution[i] = min_prob;
	}
    
    float ratio = 1.f / (1.f + added_mass);
    if (ratio < 0.999)
      {
	for (uint32_t i = 0; i < distribution.size(); i++)
	  if (distribution[i] > min_prob)
	    distribution[i] = distribution[i] * ratio; 
	safety(distribution, min_prob);
      }
  }

  void gen_cs_label(vw& all, CB::cb_class& known_cost, example& ec, COST_SENSITIVE::label& cs_ld, uint32_t label)
  {
    COST_SENSITIVE::wclass wc;
    
    //get cost prediction for this label
    wc.x = CB_ALGS::get_cost_pred<false>(all, &known_cost, ec, label, all.sd->k);
    wc.class_index = label;
    wc.partial_prediction = 0.;
    wc.wap_value = 0.;
    
    //add correction if we observed cost for this action and regressor is wrong
    if( known_cost.action == label ) 
      wc.x += (known_cost.cost - wc.x) / known_cost.probability;
    
    cs_ld.costs.push_back( wc );
  }

  template <bool is_learn>
  void predict_or_learn_cover(cbify& data, base_learner& base, example& ec)
  {//Randomize over predictions from a base set of predictors
    //Use cost sensitive oracle to cover actions to form distribution.
    MULTICLASS::label_t ld = ec.l.multi;

    data.cs_label.costs.erase();
    for (uint32_t j = 0; j < data.k; j++)
      {
	COST_SENSITIVE::wclass wc;
	
	//get cost prediction for this label
	wc.x = FLT_MAX;
	wc.class_index = j+1;
	wc.partial_prediction = 0.;
	wc.wap_value = 0.;
	data.cs_label.costs.push_back(wc);
      }

    float epsilon = data.scorer->Get_Epsilon();
    size_t cover = data.scorer->Get_Cover();
    size_t counter = data.scorer->Get_Counter();
    v_array<float>& scores = data.scorer->Get_Scores();
    v_array<uint32_t>& predictions = data.scorer->Get_Predictions();

    float additive_probability = 1.f / (float)cover;

    ec.l.cs = data.cs_label;

    float min_prob = epsilon * min(1.f / data.k, 1.f / (float)sqrt(counter * data.k));
    
    vw_context cp;
    cp.data = &data;
    cp.e = &ec;
    uint32_t action = data.mwt_explorer->Choose_Action(*data.generic_explorer.get(), to_string((unsigned long long)ec.example_counter), cp);
    
    if (is_learn)
      {
	data.cb_label.costs.erase();
  float probability = data.recorder->Get_Prob();
	CB::cb_class l = {loss(ld.label, action), 
			  action, probability};
	data.cb_label.costs.push_back(l);
	ec.l.cb = data.cb_label;
	base.learn(ec);

	//Now update oracles
	
	//1. Compute loss vector
	data.cs_label.costs.erase();
	float norm = min_prob * data.k;
	for (uint32_t j = 0; j < data.k; j++)
	  { //data.cs_label now contains an unbiased estimate of cost of each class.
	    gen_cs_label(*data.all, l, ec, data.cs_label, j+1);
      scores[j] = 0;
	  }
	
	ec.l.cs = data.second_cs_label;
	//2. Update functions
  for (size_t i = 0; i < cover; i++)
	  { //get predicted cost-sensitive predictions
	    for (uint32_t j = 0; j < data.k; j++)
	      {
    float pseudo_cost = data.cs_label.costs[j].x - epsilon * min_prob / (max(scores[j], min_prob) / norm) + 1;
		data.second_cs_label.costs[j].class_index = j+1;
		data.second_cs_label.costs[j].x = pseudo_cost;
	      }
	    if (i != 0)
	      data.cs->learn(ec,i+1);
      if (scores[predictions[i] - 1] < min_prob)
        norm += max(0, additive_probability - (min_prob - scores[predictions[i] - 1]));
	    else
	      norm += additive_probability;
      scores[predictions[i] - 1] += additive_probability;
	  }
      }

    ec.pred.multiclass = action;
    ec.l.multi = ld;
  }
  
  void init_driver(cbify&) {}

  void finish(cbify& data)
  { CB::cb_label.delete_label(&data.cb_label); }

  base_learner* cbify_setup(vw& all)
  {//parse and set arguments
    if (missing_option<size_t, true>(all, "cbify", "Convert multiclass on <k> classes into a contextual bandit problem"))
      return NULL;
    new_options(all, "CBIFY options")
      ("first", po::value<size_t>(), "tau-first exploration")
      ("epsilon",po::value<float>() ,"epsilon-greedy exploration")
      ("bag",po::value<size_t>() ,"bagging-based exploration")
      ("cover",po::value<size_t>() ,"bagging-based exploration");
    add_options(all);

    po::variables_map& vm = all.vm;
    cbify& data = calloc_or_die<cbify>();
    data.all = &all;
    data.k = (uint32_t)vm["cbify"].as<size_t>();

    if (count(all.args.begin(), all.args.end(),"--cb") == 0)
      {
	all.args.push_back("--cb");
	stringstream ss;
	ss << vm["cbify"].as<size_t>();
	all.args.push_back(ss.str());
      }
    base_learner* base = setup_base(all);
    
    learner<cbify>* l;
    data.recorder.reset(new vw_recorder());
    data.mwt_explorer.reset(new MwtExplorer<vw_context>("vw", *data.recorder.get()));
    if (vm.count("cover"))
      {
	size_t cover = (uint32_t)vm["cover"].as<size_t>();
	data.cs = all.cost_sensitive;
	data.second_cs_label.costs.resize(data.k);
	data.second_cs_label.costs.end = data.second_cs_label.costs.begin+data.k;
	float epsilon = 0.05f;
	if (vm.count("epsilon"))
	  epsilon = vm["epsilon"].as<float>();
	data.scorer.reset(new vw_cover_scorer(epsilon, cover, (u32)data.k));
	data.generic_explorer.reset(new GenericExplorer<vw_context>(*data.scorer.get(), (u32)data.k));
	l = &init_multiclass_learner(&data, base, predict_or_learn_cover<true>, 
					     predict_or_learn_cover<false>, all.p, cover + 1);
      }
    else if (vm.count("bag"))
      {
	size_t bags = (uint32_t)vm["bag"].as<size_t>();
	for (size_t i = 0; i < bags; i++)
	  {
	    data.policies.push_back(unique_ptr<IPolicy<vw_context>>(new vw_policy(i)));
	  }
	data.bootstrap_explorer.reset(new BootstrapExplorer<vw_context>(data.policies, (u32)data.k));
	l = &init_multiclass_learner(&data, base, predict_or_learn_bag<true>, 
					     predict_or_learn_bag<false>, all.p, bags);
      }
    else if (vm.count("first") )
      {
	uint32_t tau = (uint32_t)vm["first"].as<size_t>();
	data.policy.reset(new vw_policy());
	data.tau_explorer.reset(new TauFirstExplorer<vw_context>(*data.policy.get(), (u32)tau, (u32)data.k));
	l = &init_multiclass_learner(&data, base, predict_or_learn_first<true>, 
					     predict_or_learn_first<false>, all.p, 1);
      }
    else
      {
	float epsilon = 0.05f;
	if (vm.count("epsilon"))
	  epsilon = vm["epsilon"].as<float>();
	data.policy.reset(new vw_policy());
	data.greedy_explorer.reset(new EpsilonGreedyExplorer<vw_context>(*data.policy.get(), epsilon, (u32)data.k));
	l = &init_multiclass_learner(&data, base, predict_or_learn_greedy<true>, 
				     predict_or_learn_greedy<false>, all.p, 1);
      }
    l->set_finish(finish);
    l->set_init_driver(init_driver);
    
    return make_base(*l);
  }
