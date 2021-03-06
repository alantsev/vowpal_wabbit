/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <float.h>

#include "vw.h"
#include "reductions.h"
#include "cb_algs.h"

using namespace LEARNER;

#define CB_TYPE_DR 0
#define CB_TYPE_DM 1
#define CB_TYPE_IPS 2

using namespace CB;

  struct cb {
    size_t cb_type;
    size_t num_actions;
    COST_SENSITIVE::label cb_cs_ld; 
    float avg_loss_regressors;
    size_t nb_ex_regressors;
    float last_pred_reg;
    float last_correct_cost;
    
    cb_class* known_cost;
    vw* all;
  };
  
  bool know_all_cost_example(CB::label& ld)
  {
    if (ld.costs.size() <= 1) //this means we specified an example where all actions are possible but only specified the cost for the observed action
      return false;

    //if we specified more than 1 action for this example, i.e. either we have a limited set of possible actions, or all actions are specified
    //than check if all actions have a specified cost
    for (cb_class* cl = ld.costs.begin; cl != ld.costs.end; cl++)
      if (cl->cost == FLT_MAX)
        return false;

    return true;
  }

  bool is_test_label(CB::label& ld)
  {
    if (ld.costs.size() == 0)
      return true;
    for (size_t i=0; i<ld.costs.size(); i++)
      if (FLT_MAX != ld.costs[i].cost && ld.costs[i].probability > 0.)
        return false;
    return true;
  }
  
  inline bool observed_cost(cb_class* cl)
  {
    //cost observed for this action if it has non zero probability and cost != FLT_MAX
    return (cl != NULL && cl->cost != FLT_MAX && cl->probability > .0);
  }
  
  cb_class* get_observed_cost(CB::label& ld)
  {
    size_t i = 0;
    for (cb_class *cl = ld.costs.begin; cl != ld.costs.end; cl ++, i++)
    {
      if( observed_cost(cl) ) {
        return cl;
      }
    }
    return NULL;
  }

  void gen_cs_example_ips(vw& all, cb& c, example& ec, CB::label& ld, COST_SENSITIVE::label& cs_ld)
  {//this implements the inverse propensity score method, where cost are importance weighted by the probability of the chosen action
    //generate cost-sensitive example
    cs_ld.costs.erase();
    if( ld.costs.size() == 1) { //this is a typical example where we can perform all actions
      //in this case generate cost-sensitive example with all actions
      for(uint32_t i = 1; i <= c.num_actions; i++)
      {
        COST_SENSITIVE::wclass wc;
        wc.wap_value = 0.;
        wc.x = 0.;
        wc.class_index = i;
        wc.partial_prediction = 0.;
        wc.wap_value = 0.;
        if( c.known_cost != NULL && i == c.known_cost->action )
        {
          wc.x = c.known_cost->cost / c.known_cost->probability; //use importance weighted cost for observed action, 0 otherwise 
          //ips can be thought as the doubly robust method with a fixed regressor that predicts 0 costs for everything
          //update the loss of this regressor 
          c.nb_ex_regressors++;
          c.avg_loss_regressors += (1.0f/c.nb_ex_regressors)*( (c.known_cost->cost)*(c.known_cost->cost) - c.avg_loss_regressors );
          c.last_pred_reg = 0;
          c.last_correct_cost = c.known_cost->cost;
        }

        cs_ld.costs.push_back(wc );
      }
    }
    else { //this is an example where we can only perform a subset of the actions
      //in this case generate cost-sensitive example with only allowed actions
      for( cb_class* cl = ld.costs.begin; cl != ld.costs.end; cl++ )
      {
        COST_SENSITIVE::wclass wc;
        wc.wap_value = 0.;
        wc.x = 0.;
        wc.class_index = cl->action;
        wc.partial_prediction = 0.;
        wc.wap_value = 0.;
        if( c.known_cost != NULL && cl->action == c.known_cost->action )
        {
          wc.x = c.known_cost->cost / c.known_cost->probability; //use importance weighted cost for observed action, 0 otherwise 

          //ips can be thought as the doubly robust method with a fixed regressor that predicts 0 costs for everything
          //update the loss of this regressor 
          c.nb_ex_regressors++;
          c.avg_loss_regressors += (1.0f/c.nb_ex_regressors)*( (c.known_cost->cost)*(c.known_cost->cost) - c.avg_loss_regressors );
          c.last_pred_reg = 0;
          c.last_correct_cost = c.known_cost->cost;
        }

        cs_ld.costs.push_back( wc );
      }
    }

  }

  template <bool is_learn>
  void gen_cs_example_dm(vw& all, cb& c, example& ec, COST_SENSITIVE::label& cs_ld)
  {
    //this implements the direct estimation method, where costs are directly specified by the learned regressor.
    CB::label ld = ec.l.cb;

    float min = FLT_MAX;
    uint32_t argmin = 1;
    //generate cost sensitive example
    cs_ld.costs.erase();  
    if( ld.costs.size() == 1) { //this is a typical example where we can perform all actions
      //in this case generate cost-sensitive example with all actions  
      for(uint32_t i = 1; i <= c.num_actions; i++)
      {
        COST_SENSITIVE::wclass wc;
        wc.wap_value = 0.;
      
        //get cost prediction for this action
        wc.x = CB_ALGS::get_cost_pred<is_learn>(all, c.known_cost, ec, i, 0);
	if (wc.x < min)
	  {
	    min = wc.x;
	    argmin = i;
	  }

        wc.class_index = i;
        wc.partial_prediction = 0.;
        wc.wap_value = 0.;

        if( c.known_cost != NULL && c.known_cost->action == i ) {
          c.nb_ex_regressors++;
          c.avg_loss_regressors += (1.0f/c.nb_ex_regressors)*( (c.known_cost->cost - wc.x)*(c.known_cost->cost - wc.x) - c.avg_loss_regressors );
          c.last_pred_reg = wc.x;
          c.last_correct_cost = c.known_cost->cost;
        }

        cs_ld.costs.push_back( wc );
      }
    }
    else { //this is an example where we can only perform a subset of the actions
      //in this case generate cost-sensitive example with only allowed actions
      for( cb_class* cl = ld.costs.begin; cl != ld.costs.end; cl++ )
      {
        COST_SENSITIVE::wclass wc;
        wc.wap_value = 0.;
      
        //get cost prediction for this action
        wc.x = CB_ALGS::get_cost_pred<is_learn>(all, c.known_cost, ec, cl->action, 0);
	if (wc.x < min || (wc.x == min && cl->action < argmin))
	  {
	    min = wc.x;
	    argmin = cl->action;
	  }

        wc.class_index = cl->action;
        wc.partial_prediction = 0.;
        wc.wap_value = 0.;

        if( c.known_cost != NULL && c.known_cost->action == cl->action ) {
          c.nb_ex_regressors++;
          c.avg_loss_regressors += (1.0f/c.nb_ex_regressors)*( (c.known_cost->cost - wc.x)*(c.known_cost->cost - wc.x) - c.avg_loss_regressors );
          c.last_pred_reg = wc.x;
          c.last_correct_cost = c.known_cost->cost;
        }

        cs_ld.costs.push_back( wc );
      }
    }
    
    ec.pred.multiclass = argmin;
  }

  template <bool is_learn>
  void gen_cs_label(vw& all, cb& c, example& ec, COST_SENSITIVE::label& cs_ld, uint32_t label)
  {
    COST_SENSITIVE::wclass wc;
    wc.wap_value = 0.;
    
    //get cost prediction for this label
    wc.x = CB_ALGS::get_cost_pred<is_learn>(all, c.known_cost, ec, label, c.num_actions);
    wc.class_index = label;
    wc.partial_prediction = 0.;
    wc.wap_value = 0.;
    
    //add correction if we observed cost for this action and regressor is wrong
    if( c.known_cost != NULL && c.known_cost->action == label ) {
      c.nb_ex_regressors++;
      c.avg_loss_regressors += (1.0f/c.nb_ex_regressors)*( (c.known_cost->cost - wc.x)*(c.known_cost->cost - wc.x) - c.avg_loss_regressors );
      c.last_pred_reg = wc.x;
      c.last_correct_cost = c.known_cost->cost;
      wc.x += (c.known_cost->cost - wc.x) / c.known_cost->probability;
    }
    cs_ld.costs.push_back( wc );
  }

  template <bool is_learn>
  void gen_cs_example_dr(vw& all, cb& c, example& ec, CB::label& ld, COST_SENSITIVE::label& cs_ld)
  {//this implements the doubly robust method
    cs_ld.costs.erase();
    if(ld.costs.size() == 0)//a test example
      for(uint32_t i = 1; i <= c.num_actions; i++)
	cs_ld.costs.push_back({FLT_MAX,i,0.,0.});
    else if( ld.costs.size() == 1) //this is a typical example where we can perform all actions
      //in this case generate cost-sensitive example with all actions
      for(uint32_t i = 1; i <= c.num_actions; i++)
	gen_cs_label<is_learn>(all, c, ec, cs_ld, i);
    else  //this is an example where we can only perform a subset of the actions
      //in this case generate cost-sensitive example with only allowed actions
      for( cb_class* cl = ld.costs.begin; cl != ld.costs.end; cl++ )
	gen_cs_label<is_learn>(all, c, ec, cs_ld, cl->action);
  }

  template <bool is_learn>
  void predict_or_learn(cb& c, base_learner& base, example& ec) {
    vw* all = c.all;
    CB::label ld = ec.l.cb;

    c.known_cost = get_observed_cost(ld);    
    //generate a cost-sensitive example to update classifiers
    switch(c.cb_type)
    {
      case CB_TYPE_IPS:
        gen_cs_example_ips(*all,c,ec,ld,c.cb_cs_ld);
        break;
      case CB_TYPE_DM:
        gen_cs_example_dm<is_learn>(*all,c,ec,c.cb_cs_ld);
        break;
      case CB_TYPE_DR:
        gen_cs_example_dr<is_learn>(*all,c,ec,ld,c.cb_cs_ld);
        break;
      default:
        std::cerr << "Unknown cb_type specified for contextual bandit learning: " << c.cb_type << ". Exiting." << endl;
        throw exception();
    }

    if (c.cb_type != CB_TYPE_DM)
      {
	ec.l.cs = c.cb_cs_ld;

	if (is_learn)
	  base.learn(ec);
	else
	  base.predict(ec);
	
        for (size_t i=0; i<ld.costs.size(); i++)
          ld.costs[i].partial_prediction = c.cb_cs_ld.costs[i].partial_prediction;
	ec.l.cb = ld;
      }
  }

  void predict_eval(cb& c, base_learner& base, example& ec) {
    cout << "can not use a test label for evaluation" << endl;
    throw exception();
  }

  void learn_eval(cb& c, base_learner& base, example& ec) {
    vw* all = c.all;
    CB_EVAL::label ld = ec.l.cb_eval;
    
    c.known_cost = get_observed_cost(ld.event);
    
    if (c.cb_type == CB_TYPE_DR)
      gen_cs_example_dr<true>(*all, c, ec, ld.event, c.cb_cs_ld);
    else //c.cb_type == CB_TYPE_IPS
      gen_cs_example_ips(*all, c, ec, ld.event, c.cb_cs_ld);
    
    for (size_t i=0; i<ld.event.costs.size(); i++)
      ld.event.costs[i].partial_prediction = c.cb_cs_ld.costs[i].partial_prediction;

    ec.pred.multiclass = ec.l.cb_eval.action;
  }
  
  void print_update(vw& all, bool is_test, example& ec)
  {
    if (all.sd->weighted_examples >= all.sd->dump_interval && !all.quiet && !all.bfgs)
      {
        char label_buf[32];
        if (is_test)
          strcpy(label_buf," unknown");
        else
          sprintf(label_buf," known");
	char pred_buf[32];
	sprintf(pred_buf,"%8lu",(long unsigned int)ec.pred.multiclass);

	all.sd->print_update(all.holdout_set_off, all.current_pass, label_buf, pred_buf, 
			     ec.num_features, all.progress_add, all.progress_arg);
      }
  }

  void output_example(vw& all, cb& c, example& ec, CB::label& ld)
  {
    float loss = 0.;
    if (!is_test_label(ld))
      {//need to compute exact loss
	c.known_cost = get_observed_cost(ld);
        float chosen_loss = FLT_MAX;
        if( know_all_cost_example(ld) ) {
          for (cb_class *cl = ld.costs.begin; cl != ld.costs.end; cl++) 
            if (cl->action == ec.pred.multiclass)
              chosen_loss = cl->cost;
        }
        else 
          //we do not know exact cost of each action, so evaluate on generated cost-sensitive example currently stored in cb_cs_ld
          for (COST_SENSITIVE::wclass *cl = c.cb_cs_ld.costs.begin; cl != c.cb_cs_ld.costs.end; cl ++) 
            if (cl->class_index == ec.pred.multiclass)
	      {
		chosen_loss = cl->x;
		if (c.known_cost->action == ec.pred.multiclass && c.cb_type == CB_TYPE_DM) 
		  chosen_loss += (c.known_cost->cost - chosen_loss) / c.known_cost->probability;
	      }
        if (chosen_loss == FLT_MAX)
          cerr << "warning: cb predicted an invalid class" << endl;
	
        loss = chosen_loss;
      }

    all.sd->update(ec.test_only, loss, 1.f, ec.num_features);

    for (int* sink = all.final_prediction_sink.begin; sink != all.final_prediction_sink.end; sink++)
      all.print(*sink, (float)ec.pred.multiclass, 0, ec.tag);

    print_update(all, is_test_label(ld), ec);
  }

  void finish(cb& c)
  { c.cb_cs_ld.costs.delete_v(); }

  void finish_example(vw& all, cb& c, example& ec)
  {
    output_example(all, c, ec, ec.l.cb);
    VW::finish_example(all, &ec);
  }

  void eval_finish_example(vw& all, cb& c, example& ec)
  {
    output_example(all, c, ec, ec.l.cb_eval.event);
    VW::finish_example(all, &ec);
  }

  base_learner* cb_algs_setup(vw& all)
  {
    if (missing_option<size_t, true>(all, "cb", "Use contextual bandit learning with <k> costs"))
      return NULL;
    new_options(all, "CB options")
      ("cb_type", po::value<string>(), "contextual bandit method to use in {ips,dm,dr}")
      ("eval", "Evaluate a policy rather than optimizing.");
    add_options(all);

    cb& c = calloc_or_die<cb>();
    c.all = &all;

    c.num_actions = (uint32_t)all.vm["cb"].as<size_t>();

    all.sd->k = c.num_actions;

    bool eval = false;
    if (all.vm.count("eval"))
      eval = true;

    size_t problem_multiplier = 2;//default for DR
    if (all.vm.count("cb_type"))
    {
      std::string type_string;

      type_string = all.vm["cb_type"].as<std::string>();
      *all.file_options << " --cb_type " << type_string;
      
      if (type_string.compare("dr") == 0) 
	c.cb_type = CB_TYPE_DR;
      else if (type_string.compare("dm") == 0)
	{
	  if (eval)
	    {
	      cout << "direct method can not be used for evaluation --- it is biased." << endl;
	      throw exception();
	    }
	  c.cb_type = CB_TYPE_DM;
	  problem_multiplier = 1;
	}
      else if (type_string.compare("ips") == 0)
	{
	  c.cb_type = CB_TYPE_IPS;
	  problem_multiplier = 1;
	}
      else {
        std::cerr << "warning: cb_type must be in {'ips','dm','dr'}; resetting to dr." << std::endl;
        c.cb_type = CB_TYPE_DR;
      }
    }
    else {
      //by default use doubly robust
      c.cb_type = CB_TYPE_DR;
      *all.file_options << " --cb_type dr";
    }

    if (count(all.args.begin(), all.args.end(),"--csoaa") == 0)
      {
	all.args.push_back("--csoaa");
	stringstream ss;
	ss << all.vm["cb"].as<size_t>();
	all.args.push_back(ss.str());
      }

    base_learner* base = setup_base(all);
    if (eval)
      all.p->lp = CB_EVAL::cb_eval; 
    else
      all.p->lp = CB::cb_label; 

    learner<cb>* l;
    if (eval)
      {
	l = &init_learner(&c, base, learn_eval, predict_eval, problem_multiplier);
	l->set_finish_example(eval_finish_example); 
      }
    else
      {
	l = &init_learner(&c, base, predict_or_learn<true>, predict_or_learn<false>, 
			  problem_multiplier);
	l->set_finish_example(finish_example); 
      }
    // preserve the increment of the base learner since we are
    // _adding_ to the number of problems rather than multiplying.
    l->increment = base->increment; 
    
    l->set_finish(finish);
    return make_base(*l);
  }
