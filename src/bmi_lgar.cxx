#ifndef BMI_LGAR_CXX_INCLUDED
#define BMI_LGAR_CXX_INCLUDED


#include <stdio.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <iostream>
#include "../bmi/bmi.hxx"
#include "../include/bmi_lgar.hxx"
#include "../include/all.hxx"


// default verbosity is set to 'none' other option 'high' or 'low' needs to be specified in the config file
string verbosity="none";


/* The `head` pointer stores the address in memory of the first member of the linked list containing all the wetting fronts. The contents of struct wetting_front are defined in "all.h" */

struct wetting_front *head = NULL;
struct wetting_front *state_previous = NULL; // copy of the previous state used in mass balance computation

void BmiLGAR::
Initialize (std::string config_file)
{
  if (config_file.compare("") != 0 ) {
    this->state = new model_state;
    lgar_initialize(config_file, state);
  }
  
  num_giuh_ordinates = state->lgar_bmi_params.num_giuh_ordinates;


  // giuh ordinates are static and read in the lgar.cxx, and we need to have a copy of it to pass to giuh.cxx, so allocating/copying here
  giuh_ordinates = new double[num_giuh_ordinates];
  giuh_runoff_queue = new double[num_giuh_ordinates+1];
  
  for (int i=0; i<num_giuh_ordinates;i++)
    giuh_ordinates[i] = state->lgar_bmi_params.giuh_ordinates[i+1]; // note lgar uses 1-indexing

  for (int i=0; i<=num_giuh_ordinates;i++)
    giuh_runoff_queue[i] = 0.0;

}


/*
  This is the main function calling lgar subroutines for creating, moving, and merging wetting fronts.
  Calls to AET and mass balance module are also happening here
  If the model's timestep is smaller than the forcing's timestep then we take subtimesteps inside the subcycling loop
*/
void BmiLGAR::
Update()
{
  if (verbosity.compare("none") != 0) {
    std::cerr<<"*** LASAM BMI Update... ***  \n";
  }

  // if lasam is coupled to soil freeze-thaw, frozen fraction module is called
  if (state->lgar_bmi_params.sft_coupled)
    frozen_factor_hydraulic_conductivity(state->lgar_bmi_params);
  
  double mm_to_cm = 0.1; // unit conversion

  // local variables for readibility
  int subcycles = state->lgar_bmi_params.forcing_interval;
  int num_layers = state->lgar_bmi_params.num_layers;
  
  // local variables for a full timestep (i.e., timestep of the forcing data)
  // see 'struct lgar_mass_balance_variables' in all.hxx for full description of the variables
  double precip_timestep_cm = 0.0;
  double PET_timestep_cm = 0.0;
  double AET_timestep_cm = 0.0;
  double volend_timestep_cm = lgar_calc_mass_bal(state->lgar_bmi_params.cum_layer_thickness_cm); //0.0; // this should not be reset to 0.0 in the for loop
  double volin_timestep_cm = 0.0;
  double volon_timestep_cm = state->lgar_mass_balance.volon_timestep_cm;
  double volrunoff_timestep_cm = 0.0;
  double volrech_timestep_cm = 0.0;
  double surface_runoff_timestep_cm = 0.0; // direct surface runoff
  double volrunoff_giuh_timestep_cm = 0.0;
  double volQ_timestep_cm = 0.0;
  
  // local variables for a subtimestep (i.e., timestep of the model)
  double precip_subtimestep_cm;
  double precip_subtimestep_cm_per_h;
  double PET_subtimestep_cm;
  double PET_subtimestep_cm_per_h;
  double ponded_depth_subtimestep_cm;
  double AET_subtimestep_cm;
  double volstart_subtimestep_cm;
  double volend_subtimestep_cm = volend_timestep_cm; //0.0; // this should not be reset to 0.0 in the for loop
  double volin_subtimestep_cm;
  double volon_subtimestep_cm;
  double volrunoff_subtimestep_cm;
  double volrech_subtimestep_cm;
  double surface_runoff_subtimestep_cm; // direct surface runoff
  double precip_previous_subtimestep_cm;
  double volrunoff_giuh_subtimestep_cm;
  
  double subtimestep_h = state->lgar_bmi_params.timestep_h;
  int nint = state->lgar_bmi_params.nint;
  double wilting_point_psi_cm = state->lgar_bmi_params.wilting_point_psi_cm;

  // constant value used in the AET function
  double AET_thresh_Theta = 0.85;    // scaled soil moisture (0-1) above which AET=PET (fix later!)
  double AET_expon = 1.0;            // exponent that allows curvature of the rising portion of the Budyko curve (fix later!)

  double ponded_depth_max_cm = state->lgar_bmi_params.ponded_depth_max_cm;
  
  if (verbosity.compare("high") == 0) {

    std::cerr<<"Pr [cm/h] (timestep) = "<<state->lgar_bmi_input_params->precipitation_mm_per_h * mm_to_cm <<"\n";
    assert (state->lgar_bmi_input_params->precipitation_mm_per_h >= 0.0);

    std::cerr<<"Pr [cm/h] (timestep) = "<<state->lgar_bmi_input_params->PET_mm_per_h * mm_to_cm <<"\n";
    assert(state->lgar_bmi_input_params->PET_mm_per_h >=0.0);
  }

  // subcycling loop (loop over model's timestep)
  for (int cycle=1; cycle <= subcycles; cycle++) {

    if (verbosity.compare("high") == 0 || verbosity.compare("low") == 0) {
      std::cerr<<"Subcycle | --------------------------------------------- : "<< cycle <<" of "<<subcycles<<std::endl;
    }
    
    state_previous = NULL;
    state_previous = listCopy(head);

    // ensure precip and PET are non-negative
    state->lgar_bmi_input_params->precipitation_mm_per_h = fmax(state->lgar_bmi_input_params->precipitation_mm_per_h, 0.0);
    state->lgar_bmi_input_params->PET_mm_per_h = fmax(state->lgar_bmi_input_params->PET_mm_per_h, 0.0);

    /* Note unit conversion:
       Pr and PET are rates (fluxes) in mm/h
       Pr [mm/h] * 1h/3600sec = Pr [mm/3600sec]
       Model timestep (dt) = 300 sec (5 minutes for example)
       convert rate to amount
       Pr [mm/3600sec] * dt [300 sec] = Pr[mm] * 300/3600.
       in the code below, subtimestep_h is this 300/3600 factor (see initialize from config in lgar.cxx)
    */
    
    precip_subtimestep_cm_per_h = state->lgar_bmi_input_params->precipitation_mm_per_h * mm_to_cm; // rate [cm/hour]

    PET_subtimestep_cm_per_h = state->lgar_bmi_input_params->PET_mm_per_h * mm_to_cm;

    ponded_depth_subtimestep_cm = precip_subtimestep_cm_per_h * subtimestep_h; // the amount of water on the surface before any infiltration and runoff

    ponded_depth_subtimestep_cm += volon_timestep_cm; // add volume of water on the surface (from the last timestep) to ponded depth as well

    precip_subtimestep_cm = precip_subtimestep_cm_per_h * subtimestep_h; // rate x dt = amount (portion of the water on the suface for model's timestep [cm])
    PET_subtimestep_cm = PET_subtimestep_cm_per_h * subtimestep_h;      // potential ET for this subtimestep [cm]

    //using cerr instead of cout due to some cout buffering issues when running in the ngen framework, cerr doesn't buffer so it prints immediately to the sreeen.
    if (verbosity.compare("high") == 0 || verbosity.compare("low") == 0) {

      std::cerr<<"Pr [cm/h], Pr [cm] (subtimestep), subtimestep [h] = "<<state->lgar_bmi_input_params->precipitation_mm_per_h * mm_to_cm <<", "<< precip_subtimestep_cm <<", "<< subtimestep_h<<" ("<<subtimestep_h*3600<<" sec)"<<"\n";
      std::cerr<<"PET [cm/h], PET [cm] (subtimestep) = "<<state->lgar_bmi_input_params->PET_mm_per_h * mm_to_cm <<", "<< PET_subtimestep_cm<<"\n";
    }
    
    AET_subtimestep_cm = 0.0;
    volstart_subtimestep_cm = 0.0;
    volin_subtimestep_cm = 0.0;
    //    volon_subtimestep_cm= 0.0 + volon_timestep_cm;
    volrunoff_subtimestep_cm = 0.0;
    volrech_subtimestep_cm = 0.0;
    surface_runoff_subtimestep_cm = 0.0;

    precip_previous_subtimestep_cm = state->lgar_bmi_params.precip_previous_timestep_cm; // creation of a new wetting front depends on previous timestep's rainfall
    
    num_layers = state->lgar_bmi_params.num_layers;
    double delta_theta;   // the width of a front, such that its volume=depth*delta_theta
    double dry_depth;
    

    // Calculate AET from PET if PET is non-zero
    if (PET_subtimestep_cm_per_h > 0.0) {
      AET_subtimestep_cm = calc_aet(PET_subtimestep_cm_per_h, subtimestep_h, wilting_point_psi_cm,
				    state->lgar_bmi_params.layer_soil_type, AET_thresh_Theta, AET_expon,
				    state->soil_properties);
    }
    
    
    precip_timestep_cm += precip_subtimestep_cm;
    PET_timestep_cm += fmax(PET_subtimestep_cm,0.0); // ensures non-negative PET
    
    volstart_subtimestep_cm = lgar_calc_mass_bal(state->lgar_bmi_params.cum_layer_thickness_cm);

    
    int soil_num = state->lgar_bmi_params.layer_soil_type[head->layer_num];
    double theta_e = state->soil_properties[soil_num].theta_e;
    bool is_top_wf_saturated = head->theta >= theta_e ? true : false;
    bool create_surficial_front = (precip_previous_subtimestep_cm == 0.0 && precip_subtimestep_cm > 0.0);
    
    int wf_free_drainage_demand = wetting_front_free_drainage();

    if (verbosity.compare("high") == 0 || verbosity.compare("low") == 0) {
      std::string flag = (create_surficial_front && !is_top_wf_saturated) == true ? "Yes" : "No";
      std::cerr<<"Create superficial wetting front? "<< flag << "\n";
    }

    /*----------------------------------------------------------------------*/
    /* create a new wetting front if the following is true. Meaning there is no
       wetting front in the top layer to accept the water, must create one. */
    if(create_surficial_front && !is_top_wf_saturated)  {
      
      double temp_pd = 0.0; // necessary to assign zero precip due to the creation of new wetting front; AET will still be taken out of the layers

      // move the wetting fronts without adding any water; this is done to close the mass balance
      lgar_move_wetting_fronts(subtimestep_h, &temp_pd, wf_free_drainage_demand, volend_subtimestep_cm,
			       num_layers, &AET_subtimestep_cm, state->lgar_bmi_params.cum_layer_thickness_cm,
			       state->lgar_bmi_params.layer_soil_type, state->lgar_bmi_params.frozen_factor,
			       state->soil_properties);

      // depth of the surficial front to be created
      dry_depth = lgar_calc_dry_depth(nint, subtimestep_h, &delta_theta, state->lgar_bmi_params.layer_soil_type,
				      state->lgar_bmi_params.cum_layer_thickness_cm, state->lgar_bmi_params.frozen_factor,
				      state->soil_properties);
      
      lgar_create_surfacial_front(&ponded_depth_subtimestep_cm, &volin_subtimestep_cm, dry_depth, head->theta,
				  state->lgar_bmi_params.layer_soil_type, state->lgar_bmi_params.cum_layer_thickness_cm,
				  state->lgar_bmi_params.frozen_factor, state->soil_properties);
      
      state_previous = NULL;
      state_previous = listCopy(head);
      
      volin_timestep_cm += volin_subtimestep_cm;

      if (verbosity.compare("high") == 0) {
	std::cerr<<"New wetting front created...\n";
	std::cerr<<" "<<"\n";
	listPrint();
      }
    }
    
    /*----------------------------------------------------------------------*/
    /* infiltrate water based on the infiltration capacity given no new wetting front
       is created and that there is water on the surface (or raining). */

    if (ponded_depth_subtimestep_cm > 0 && !create_surficial_front) {
      
      volrunoff_subtimestep_cm = lgar_insert_water(nint, subtimestep_h, &ponded_depth_subtimestep_cm, &volin_subtimestep_cm,
						   precip_subtimestep_cm_per_h, wf_free_drainage_demand, num_layers,
						   ponded_depth_max_cm, state->lgar_bmi_params.layer_soil_type,
						   state->lgar_bmi_params.cum_layer_thickness_cm,
						   state->lgar_bmi_params.frozen_factor, state->soil_properties);

      volin_timestep_cm += volin_subtimestep_cm;
      volrunoff_timestep_cm += volrunoff_subtimestep_cm;
      volrech_subtimestep_cm = volin_subtimestep_cm; // this gets updated later, probably not needed here
      
      volon_subtimestep_cm = ponded_depth_subtimestep_cm;

      if (volrunoff_subtimestep_cm < 0) abort();  
    }
    else {
      
      if (ponded_depth_subtimestep_cm < ponded_depth_max_cm) {
	volrunoff_timestep_cm += 0.0;
	volon_subtimestep_cm = ponded_depth_subtimestep_cm;
	ponded_depth_subtimestep_cm = 0.0;
	volrunoff_subtimestep_cm = 0.0;
      }
      else {
	volrunoff_subtimestep_cm = (ponded_depth_subtimestep_cm - ponded_depth_max_cm);
	volrunoff_timestep_cm += (ponded_depth_subtimestep_cm - ponded_depth_max_cm);
	volon_subtimestep_cm = ponded_depth_max_cm;
	ponded_depth_subtimestep_cm = ponded_depth_max_cm;
      }
    }
    /*----------------------------------------------------------------------*/

    /* move wetting fronts if no new wetting front is created. Otherwise, movement
       of wetting fronts has already happened at the time of creating surficial front,
       so no need to move them here. */
    if (!create_surficial_front) {
      double volin_subtimestep_cm_temp = volin_subtimestep_cm;  // passing this for mass balance only, the method modifies it and returns percolated value, so we need to keep its original value stored to copy it back
      lgar_move_wetting_fronts(subtimestep_h, &volin_subtimestep_cm, wf_free_drainage_demand, volend_subtimestep_cm,
			       num_layers, &AET_subtimestep_cm, state->lgar_bmi_params.cum_layer_thickness_cm,
			       state->lgar_bmi_params.layer_soil_type, state->lgar_bmi_params.frozen_factor,
			       state->soil_properties);
      
      // this is the volume of water leaving through the bottom
      volrech_subtimestep_cm = volin_subtimestep_cm;
      volrech_timestep_cm += volrech_subtimestep_cm;

      volin_subtimestep_cm = volin_subtimestep_cm_temp;
    }
    
    /*----------------------------------------------------------------------*/
    // calculate derivative (dz/dt) for all wetting fronts
    lgar_dzdt_calc(nint, ponded_depth_subtimestep_cm, state->lgar_bmi_params.layer_soil_type,
		   state->lgar_bmi_params.cum_layer_thickness_cm, state->lgar_bmi_params.frozen_factor,
		   state->soil_properties);

    AET_timestep_cm += AET_subtimestep_cm;
    volrech_timestep_cm += volrech_subtimestep_cm;
    
    volend_subtimestep_cm = lgar_calc_mass_bal(state->lgar_bmi_params.cum_layer_thickness_cm);
    volend_timestep_cm = volend_subtimestep_cm;
    state->lgar_bmi_params.precip_previous_timestep_cm = precip_subtimestep_cm;

    /*----------------------------------------------------------------------*/
    // mass balance at the subtimestep (local mass balance)
    double local_mb = volstart_subtimestep_cm + precip_subtimestep_cm + volon_timestep_cm - volrunoff_subtimestep_cm
                      - AET_subtimestep_cm - volon_subtimestep_cm - volrech_subtimestep_cm - volend_subtimestep_cm;

    volon_timestep_cm = volon_subtimestep_cm; // surface ponded water at the end of the timestep
    
    /*----------------------------------------------------------------------*/
    // compute giuh runoff for the subtimestep
    surface_runoff_subtimestep_cm = volrunoff_subtimestep_cm;
    volrunoff_giuh_subtimestep_cm = giuh_convolution_integral(volrunoff_subtimestep_cm, num_giuh_ordinates, giuh_ordinates, giuh_runoff_queue);

    surface_runoff_timestep_cm += surface_runoff_subtimestep_cm ;
    volrunoff_giuh_timestep_cm += volrunoff_giuh_subtimestep_cm;

    // total mass of water leaving the system, at this time it is the giuh-only, but later will add groundwater component as well.

    volQ_timestep_cm += volrunoff_giuh_subtimestep_cm;

    if (verbosity.compare("high") == 0 || verbosity.compare("low") == 0) {
      printf("Printing wetting fronts at this subtimestep... \n");
      listPrint();
    }

    bool unexpected_local_error = fabs(local_mb) > 1.0e-7 ? true : false;
    
    if (verbosity.compare("high") == 0 || verbosity.compare("low") == 0 || unexpected_local_error) {
      printf("\nLocal mass balance at this timestep... \n\
      Error         = %14.10f \n\
      Initial water = %14.10f \n\
      Water added   = %14.10f \n\
      Ponded water  = %14.10f \n\
      Infiltration  = %14.10f \n\
      Runoff        = %14.10f \n\
      AET           = %14.10f \n\
      Percolation   = %14.10f \n\
      Final water   = %14.10f \n", local_mb, volstart_subtimestep_cm, precip_subtimestep_cm, volon_subtimestep_cm, volin_subtimestep_cm, volrunoff_subtimestep_cm, AET_subtimestep_cm, volrech_subtimestep_cm, volend_subtimestep_cm);

      if (unexpected_local_error) {
	printf("Local mass balance (in this timestep) is %14.10f, larger than expected, needs some debugging...\n ",local_mb);
	abort();
      }
	
    }
    
    assert (head->depth_cm > 0.0); // check on negative layer depth


    this->state->lgar_bmi_params.time_s += subtimestep_h * state->units.hr_to_sec; // convert hour to seconds (AJ: fix this 3600 hacked value)

    bool lasam_standalone = true;
#ifdef NGEN
    lasam_standalone = false;
#endif
    // simuation time can't exceed the endtime when running standalone
    if ( (this->state->lgar_bmi_params.time_s >= this->state->lgar_bmi_params.endtime_s) && lasam_standalone)
      break;
    
  } // end of subcycling

  /*----------------------------------------------------------------------*/
  // Everything related to lgar state is done this point, now time to update some dynamic variables
  
  // update number of wetting fronts
  state->lgar_bmi_params.num_wetting_fronts = listLength();

  // allocate new memory based on updated wetting fronts; we could make it conditional i.e. create only if no. of wf are changed
  state->lgar_bmi_params.soil_thickness_wetting_fronts = new double[state->lgar_bmi_params.num_wetting_fronts];
  state->lgar_bmi_params.soil_moisture_wetting_fronts = new double[state->lgar_bmi_params.num_wetting_fronts];

  // update thickness/depth and soil moisture of wetting fronts (used for state coupling)
  struct wetting_front *current = head;
  for (int i=0; i<state->lgar_bmi_params.num_wetting_fronts; i++) {
    assert (current != NULL);
    state->lgar_bmi_params.soil_moisture_wetting_fronts[i] = current->theta;
    state->lgar_bmi_params.soil_thickness_wetting_fronts[i] = current->depth_cm * state->units.cm_to_m;
    current = current->next;
    if (verbosity.compare("high") == 0)
      std::cerr<<"Wetting fronts (bmi outputs) (depth in meters, theta)= "<<state->lgar_bmi_params.soil_thickness_wetting_fronts[i]
	       <<" "<<state->lgar_bmi_params.soil_moisture_wetting_fronts[i]<<"\n";
  }
  
  // add to mass balance timestep variables
  state->lgar_mass_balance.volprecip_timestep_cm = precip_timestep_cm;
  state->lgar_mass_balance.volin_timestep_cm = volin_timestep_cm;
  state->lgar_mass_balance.volon_timestep_cm = volon_timestep_cm;
  state->lgar_mass_balance.volend_timestep_cm = volend_timestep_cm;
  state->lgar_mass_balance.volAET_timestep_cm = AET_timestep_cm;
  state->lgar_mass_balance.volrech_timestep_cm = volrech_timestep_cm;
  state->lgar_mass_balance.volrunoff_timestep_cm = volrunoff_timestep_cm;
  state->lgar_mass_balance.volrunoff_giuh_timestep_cm = volrunoff_giuh_timestep_cm;
  state->lgar_mass_balance.volQ_timestep_cm = volQ_timestep_cm;
  state->lgar_mass_balance.volPET_timestep_cm = PET_timestep_cm;
  
  // add to mass balance accumulated variables
  state->lgar_mass_balance.volprecip_cm += precip_timestep_cm;
  state->lgar_mass_balance.volin_cm += volin_timestep_cm;
  state->lgar_mass_balance.volon_cm = volon_timestep_cm;
  state->lgar_mass_balance.volend_cm = volend_timestep_cm;
  state->lgar_mass_balance.volAET_cm += AET_timestep_cm;
  state->lgar_mass_balance.volrech_cm += volrech_timestep_cm;
  state->lgar_mass_balance.volrunoff_cm += volrunoff_timestep_cm;
  state->lgar_mass_balance.volrunoff_giuh_cm += volrunoff_giuh_timestep_cm;
  state->lgar_mass_balance.volQ_cm += volQ_timestep_cm;
  state->lgar_mass_balance.volPET_cm += PET_timestep_cm;


  // converted values, a struct local to the BMI and has bmi output variables
  bmi_unit_conv.volprecip_timestep_m = precip_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volin_timestep_m = volin_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volend_timestep_m = volend_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volAET_timestep_m = AET_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volrech_timestep_m = volrech_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volrunoff_timestep_m = volrunoff_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volrunoff_giuh_timestep_m = volrunoff_giuh_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volQ_timestep_m = volQ_timestep_cm * state->units.cm_to_m;
  bmi_unit_conv.volPET_timestep_m = PET_timestep_cm * state->units.cm_to_m;

  // Update time
  //this->state->lgar_bmi_params.time_s += this->state->lgar_bmi_params.forcing_resolution_h * state->units.hr_to_sec; // convert hour to seconds (AJ: fix this 3600 hacked value)
  
  if (verbosity.compare("high") == 0)
    std::cerr<<"Current time = "<<this->state->lgar_bmi_params.time_s / state->units.hr_to_sec <<" hours \n";

}


void BmiLGAR::
UpdateUntil(double t)
{
  assert (t > 0.0);
  this->Update();
}

struct model_state* BmiLGAR::get_model()
{
  return state;
}

void BmiLGAR::
global_mass_balance()
{
  lgar_global_mass_balance(this->state, giuh_runoff_queue);
}

void BmiLGAR::
Finalize()
{
  global_mass_balance();
}


int BmiLGAR::
GetVarGrid(std::string name)
{
  if (name.compare("soil_storage_model") == 0 || name.compare("soil_num_wetting_fronts") == 0)   // int
    return 0;
  else if (name.compare("precipitation_rate") == 0 || name.compare("precipitation") == 0)
    return 1;
  else if (name.compare("potential_evapotranspiration_rate") == 0 || name.compare("potential_evapotranspiration") == 0
	   || name.compare("actual_evapotranspiration") == 0) // double
    return 1;
  else if (name.compare("surface_runoff") == 0 || name.compare("giuh_runoff") == 0 || name.compare("soil_storage") == 0) // double
    return 1;
  else if (name.compare("total_discharge") == 0 || name.compare("infiltration") == 0 || name.compare("percolation") == 0) // double
    return 1;
  else if (name.compare("soil_moisture_layers") == 0 || name.compare("soil_thickness_layers") == 0) // array of doubles (fixed length)
    return 2;
  else if (name.compare("soil_moisture_wetting_fronts") == 0 || name.compare("soil_thickness_wetting_fronts") == 0) // array of doubles (dynamic length)
    return 3;
  else if (name.compare("soil_temperature_profile") == 0) // array of doubles (fixed and of the size of soil temperature profile)
    return 4; 
  else 
    return -1;
}


std::string BmiLGAR::
GetVarType(std::string name)
{
  int var_grid = GetVarGrid(name);
  
  if (var_grid == 0)
    return "int";
  else if (var_grid == 1 || var_grid == 2 || var_grid == 3 || var_grid == 4) 
    return "double";
  else
    return "none";
}


int BmiLGAR::
GetVarItemsize(std::string name)
{
  int var_grid = GetVarGrid(name);

   if (var_grid == 0)
    return sizeof(int);
  else if (var_grid == 1 || var_grid == 2 || var_grid == 3 || var_grid == 4)
    return sizeof(double);
  else
    return 0;
}


std::string BmiLGAR::
GetVarUnits(std::string name)
{
  if (name.compare("precipitation_rate") == 0 || name.compare("potential_evapotranspiration_rate") == 0)
    return "mm h^-1";
  else if (name.compare("precipitation") == 0 || name.compare("potential_evapotranspiration") == 0 || name.compare("actual_evapotranspiration") == 0) // double
    return "m";
  else if (name.compare("surface_runoff") == 0 || name.compare("giuh_runoff") == 0 || name.compare("soil_storage") == 0) // double
    return "m";
   else if (name.compare("total_discharge") == 0 || name.compare("infiltration") == 0 || name.compare("percolation") == 0) // double
    return "m";
  else if (name.compare("soil_moisture_layers") == 0 || name.compare("soil_moisture_wetting_fronts") == 0) // array of doubles 
    return "none";
  else if (name.compare("soil_thickness_layers") == 0 || name.compare("soil_thickness_wetting_fronts") == 0) // array of doubles 
    return "m";
  else if (name.compare("soil_temperature_profile") == 0)
    return "K";
  else 
    return "none";
  
}


int BmiLGAR::
GetVarNbytes(std::string name)
{
  int itemsize;
  int gridsize;

  itemsize = this->GetVarItemsize(name);
  gridsize = this->GetGridSize(this->GetVarGrid(name));
  return itemsize * gridsize;
}


std::string BmiLGAR::
GetVarLocation(std::string name)
{
  if (name.compare("precipitation_rate") == 0 || name.compare("precipitation") == 0 ||
      name.compare("potential_evapotranspiration") == 0 || name.compare("potential_evapotranspiration_rate") == 0
      || name.compare("actual_evapotranspiration") == 0) // double
    return "node";
  else if (name.compare("surface_runoff") == 0 || name.compare("giuh_runoff") == 0 || name.compare("soil_storage") == 0) // double
    return "node";
   else if (name.compare("total_discharge") == 0 || name.compare("infiltration") == 0 || name.compare("percolation") == 0) // double
    return "node";
  else if (name.compare("soil_moisture_layers") == 0 || name.compare("soil_moisture_wetting_fronts") == 0) // array of doubles 
    return "node";
  else if (name.compare("soil_thickness_layers") == 0 || name.compare("soil_thickness_wetting_fronts") == 0 || name.compare("soil_num_wetting_fronts") == 0) // array of doubles 
    return "node";
  else if (name.compare("soil_temperature_profile") == 0)
    return "node";
  else 
    return "none";
}


void BmiLGAR::
GetGridShape(const int grid, int *shape)
{
  if (grid == 2)
    shape[0] = this->state->lgar_bmi_params.num_layers;
  else if (grid == 3) // number of wetting fronts (dynamic)
    shape[1] = this->state->lgar_bmi_params.num_wetting_fronts;
}


void BmiLGAR::
GetGridSpacing (const int grid, double * spacing)
{
  if (grid == 0) {
    spacing[0] = this->state->lgar_bmi_params.spacing[0];
  }
}


void BmiLGAR::
GetGridOrigin (const int grid, double *origin)
{
  if (grid == 0) {
    origin[0] = this->state->lgar_bmi_params.origin[0];
  }
}


int BmiLGAR::
GetGridRank(const int grid)
{
  if (grid == 0 || grid == 1 || grid == 2 || grid == 3 || grid == 4)
    return 1;
  else
    return -1;
}


int BmiLGAR::
GetGridSize(const int grid)
{
  if (grid == 0 || grid == 1)
    return 1;
  else if (grid == 2) // number of layers (fixed)
    return this->state->lgar_bmi_params.num_layers;
  else if (grid == 3) // number of wetting fronts (dynamic)
    return this->state->lgar_bmi_params.num_wetting_fronts;
  else if (grid == 4) // number of cells (discretized temperature profile, input from SFT)
    return this->state->lgar_bmi_params.num_cells_temp;
  else
    return -1;
}



void BmiLGAR::
GetValue (std::string name, void *dest)
{
  void * src = NULL;
  int nbytes = 0;

  src = this->GetValuePtr(name);
  nbytes = this->GetVarNbytes(name);
  memcpy (dest, src, nbytes);
}


void *BmiLGAR::
GetValuePtr (std::string name)
{
  if (name.compare("precipitation_rate") == 0)
    return (void*)(&this->state->lgar_bmi_input_params->precipitation_mm_per_h);
  else if (name.compare("precipitation") == 0)
    return (void*)(&bmi_unit_conv.volprecip_timestep_m);
  else if (name.compare("potential_evapotranspiration_rate") == 0)
    return (void*)(&this->state->lgar_bmi_input_params->PET_mm_per_h);
  else if (name.compare("potential_evapotranspiration") == 0)
    return (void*)(&bmi_unit_conv.volPET_timestep_m);
  else if (name.compare("actual_evapotranspiration") == 0)
    return (void*)(&bmi_unit_conv.volAET_timestep_m);
  else if (name.compare("surface_runoff") == 0)
    return (void*)(&bmi_unit_conv.volrunoff_timestep_m);
  else if (name.compare("giuh_runoff") == 0)
    return (void*)(&bmi_unit_conv.volrunoff_giuh_timestep_m);
  else if (name.compare("soil_storage") == 0)
    return (void*)(&bmi_unit_conv.volend_timestep_m);
  else if (name.compare("total_discharge") == 0)
    return (void*)(&bmi_unit_conv.volQ_timestep_m);
  else if (name.compare("infiltration") == 0)
    return (void*)(&bmi_unit_conv.volin_timestep_m);
  else if (name.compare("percolation") == 0)
    return (void*)(&bmi_unit_conv.volrech_timestep_m);
  else if (name.compare("soil_moisture_layers") == 0)
    return (void*)this->state->lgar_bmi_params.soil_moisture_layers; // this is probably not needed
  else if (name.compare("soil_thickness_layers") == 0)
    return (void*)this->state->lgar_bmi_params.soil_moisture_layers;  // this too and, if needed, change soil_moisture_layers to soil_thickness_layers
  else if (name.compare("soil_moisture_wetting_fronts") == 0)
    return (void*)this->state->lgar_bmi_params.soil_moisture_wetting_fronts;
  else if (name.compare("soil_thickness_wetting_fronts") == 0)
    return (void*)this->state->lgar_bmi_params.soil_thickness_wetting_fronts;
  else if (name.compare("soil_num_wetting_fronts") == 0)
    return (void*)(&state->lgar_bmi_params.num_wetting_fronts);
  else if (name.compare("soil_temperature_profile") == 0)
    return (void*)this->state->lgar_bmi_params.soil_temperature;
  else {
    std::stringstream errMsg;
    errMsg << "variable "<< name << " does not exist";
    throw std::runtime_error(errMsg.str());
    return NULL;
    }
  
  // delete it later
  return NULL;
}

void BmiLGAR::
GetValueAtIndices (std::string name, void *dest, int *inds, int len)
{
  void * src = NULL;

  src = this->GetValuePtr(name);

  if (src) {
    int i;
    int itemsize = 0;
    int offset;
    char *ptr;

    itemsize = this->GetVarItemsize(name);

    for (i=0, ptr=(char *)dest; i<len; i++, ptr+=itemsize) {
      offset = inds[i] * itemsize;
      memcpy(ptr, (char *)src + offset, itemsize);
    }
  }
}


void BmiLGAR::
SetValue (std::string name, void *src)
{
  void * dest = NULL;
  dest = this->GetValuePtr(name);

  if (dest) {
    int nbytes = 0;
    nbytes = this->GetVarNbytes(name);
    memcpy(dest, src, nbytes);
  }

}


void BmiLGAR::
SetValueAtIndices (std::string name, int * inds, int len, void *src)
{
  void * dest = NULL;

  dest = this->GetValuePtr(name);

  if (dest) {
    int i;
    int itemsize = 0;
    int offset;
    char *ptr;

    itemsize = this->GetVarItemsize(name);

    for (i=0, ptr=(char *)src; i<len; i++, ptr+=itemsize) {
      offset = inds[i] * itemsize;
      memcpy((char *)dest + offset, ptr, itemsize);
    }
  }
}


std::string BmiLGAR::
GetComponentName()
{
  return "LASAM (Lumped Arid/Semi-arid Model)";
}


int BmiLGAR::
GetInputItemCount()
{
  return this->input_var_name_count;
}


int BmiLGAR::
GetOutputItemCount()
{
  return this->output_var_name_count;
}


std::vector<std::string> BmiLGAR::
GetInputVarNames()
{
  std::vector<std::string> names;
  
  for (int i=0; i<this->input_var_name_count; i++)
    names.push_back(this->input_var_names[i]);
  
  return names;
}


std::vector<std::string> BmiLGAR::
GetOutputVarNames()
{
  std::vector<std::string> names;

  for (int i=0; i<this->output_var_name_count; i++)
    names.push_back(this->output_var_names[i]);

  return names;
}


double BmiLGAR::
GetStartTime () {
  return 0.0;
}


double BmiLGAR::
GetEndTime () {
  return this->state->lgar_bmi_params.endtime_s;
}


double BmiLGAR::
GetCurrentTime () {
  return this->state->lgar_bmi_params.time_s;
}


std::string BmiLGAR::
GetTimeUnits() {
  return "s";
}


double BmiLGAR::
GetTimeStep () {
  return this->state->lgar_bmi_params.forcing_resolution_h * 3600; // convert hours to seconds
}

std::string BmiLGAR::
GetGridType(const int grid)
{
  if (grid == 0)
    return "uniform_rectilinear";
  else
    return "";
}


void BmiLGAR::
GetGridX(const int grid, double *x)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridX: "<<grid<<" "<<x[0]<<"\n"; 
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridY(const int grid, double *y)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridY: "<<grid<<" "<<y[0]<<"\n"; 
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridZ(const int grid, double *z)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridZ: "<<grid<<" "<<z[0]<<"\n"; 
  throw bmi_lgar::NotImplemented();
}


int BmiLGAR::
GetGridNodeCount(const int grid)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridNodeCount: "<<grid<<"\n"; 
  throw bmi_lgar::NotImplemented();
  /*
  if (grid == 0)
    return this->model->shape[0];
  else
    return -1;
  */
}


int BmiLGAR::
GetGridEdgeCount(const int grid)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridEdgeCount: "<<grid<<"\n";
  throw bmi_lgar::NotImplemented();
}


int BmiLGAR::
GetGridFaceCount(const int grid)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridFaceCount: "<<grid<<"\n";
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridEdgeNodes(const int grid, int *edge_nodes)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridEdgeNodes: "<<grid<<" "<<edge_nodes[0]<<"\n";
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridFaceEdges(const int grid, int *face_edges)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridFaceNodes: "<<grid<<" "<<face_edges[0]<<"\n";
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridFaceNodes(const int grid, int *face_nodes)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridFaceNodes: "<<grid<<" "<<face_nodes[0]<<"\n";
  throw bmi_lgar::NotImplemented();
}


void BmiLGAR::
GetGridNodesPerFace(const int grid, int *nodes_per_face)
{
  // this is not needed but printing here to avoid compiler warnings
  std::cerr<<"GetGridNodesPerFace: "<<grid<<" "<<nodes_per_face[0]<<"\n";
  throw bmi_lgar::NotImplemented();
}

#endif
