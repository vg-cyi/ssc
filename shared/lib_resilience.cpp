/*
BSD 3-Clause License

Copyright (c) Alliance for Sustainable Energy, LLC. See also https://github.com/NREL/ssc/blob/develop/LICENSE
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "lib_resilience.h"

dispatch_resilience::dispatch_resilience(const dispatch_t &orig, size_t start_index) :
        dispatch_t(orig),
        connection(static_cast<CONNECTION>(m_batteryPower->connectionMode)),
        start_outage_index(start_index){
    inverter = nullptr;
    if (connection == CONNECTION::DC_CONNECTED)
        inverter = std::unique_ptr<SharedInverter>(new SharedInverter(*m_batteryPower->sharedInverter));
    current_outage_index = start_outage_index;
    met_loads_kw = 0;

    m_batteryPower->canClipCharge = true;
    m_batteryPower->canSystemCharge = true;
    m_batteryPower->canGridCharge = false;
    m_batteryPower->canDischarge = true;

    // change SOC limits
    _Battery->changeSOCLimits(orig._min_outage_soc, 100.);
    m_batteryPower->stateOfChargeMin = orig._min_outage_soc;
    m_batteryPower->stateOfChargeMax = 100;
}

dispatch_resilience::~dispatch_resilience() {
	delete_clone();
	_Battery_initial = nullptr;
}

bool dispatch_resilience::run_outage_step_ac(double crit_load_kwac, double pv_kwac){
    if (connection != CONNECTION::AC_CONNECTED)
        throw std::runtime_error("Error in resilience::run_outage_step_ac: called for battery with DC connection.");
    m_batteryPower->reset();
    if (pv_kwac < 0) {
        m_batteryPower->powerPVInverterDraw = m_batteryPower->powerSystem;
        m_batteryPower->powerSystem = 0;
    }
    else {
        m_batteryPower->powerPVInverterDraw = 0;
        m_batteryPower->powerSystem = pv_kwac;
    }
    m_batteryPower->powerCritLoad = crit_load_kwac;
    m_batteryPower->isOutageStep = true;

    dispatch_ac_outage_step(current_outage_index);

    double met_load = m_batteryPower->powerBatteryToLoad + m_batteryPower->powerSystemToLoad + m_batteryPower->powerFuelCellToLoad;
    double unmet_load = m_batteryPower->powerCritLoadUnmet;
    met_loads_kw += met_load;
    bool survived = unmet_load < tolerance;
    if (survived)
        current_outage_index += 1;
    return survived;
}

bool dispatch_resilience::run_outage_step_dc(double crit_load_kwac, double pv_kwdc, double V_pv, double pv_clipped, double tdry) {
    if (connection != CONNECTION::DC_CONNECTED)
        throw std::runtime_error("Error in resilience::run_outage_step_dc: called for battery with AC connection.");
    m_batteryPower->reset();
    m_batteryPower->powerSystem = pv_kwdc;
    m_batteryPower->powerCritLoad = crit_load_kwac;
    m_batteryPower->voltageSystem = V_pv;
    m_batteryPower->powerSystemClipped = pv_clipped;
    m_batteryPower->sharedInverter->Tdry_C = tdry;
    m_batteryPower->isOutageStep = true;

    dispatch_dc_outage_step(current_outage_index);

    double met_load = m_batteryPower->powerBatteryToLoad + m_batteryPower->powerSystemToLoad + m_batteryPower->powerFuelCellToLoad;
    double unmet_load = m_batteryPower->powerCritLoadUnmet;
    met_loads_kw += met_load;
    bool survived = unmet_load < tolerance;
    if (survived)
        current_outage_index += 1;
    return survived;
}

size_t dispatch_resilience::get_indices_survived() {
    return current_outage_index - start_outage_index;
}

double dispatch_resilience::get_met_loads(){
    return met_loads_kw;
}

resilience_runner::resilience_runner(const std::shared_ptr<battstor>& battery)
{
    batt = battery;
    size_t steps_lifetime = batt->step_per_hour * batt->nyears * 8760;
    indices_survived.resize(steps_lifetime);
    total_load_met.resize(steps_lifetime);
}

void resilience_runner::add_battery_at_outage_timestep(const dispatch_t& orig, size_t index){
    if (battery_per_outage_start.find(index) != battery_per_outage_start.end())
        logs.emplace_back(
                "Replacing battery which already existed at index " + to_string(index) + ".");
    battery_per_outage_start.insert({index, std::make_shared<dispatch_resilience>(orig, index)});
}

void resilience_runner::run_surviving_batteries(double crit_loads_kwac, double pv_kwac, double pv_kwdc, double V,
                                                double pv_clipped_kw, double tdry_c) {
    if (batt->batt_vars->batt_topology == dispatch_resilience::DC_CONNECTED) {
        if (batt->batt_vars->inverter_paco * batt->batt_vars->inverter_count < crit_loads_kwac)
            logs.emplace_back(
                    "For DC-connected battery, maximum inverter AC Power less than max load will lead to dropped load.");
    }

    std::vector<size_t> depleted_battery_keys;
    for (auto& i : battery_per_outage_start){
        size_t start_index = i.first;
        auto batt_system = i.second;
        bool survived;
        if (batt_system->connection == dispatch_resilience::DC_CONNECTED)
            survived = batt_system->run_outage_step_dc(crit_loads_kwac, pv_kwdc, V, pv_clipped_kw, tdry_c);
        else
            survived = batt_system->run_outage_step_ac(crit_loads_kwac, pv_kwac);
        if (!survived){
            depleted_battery_keys.emplace_back(start_index);
            indices_survived[start_index] = batt_system->get_indices_survived();
        }
    }
    for (auto& i : depleted_battery_keys){
        auto b = battery_per_outage_start[i];
        indices_survived[i] = b->get_indices_survived();
        total_load_met[i] = b->get_met_loads();
        battery_per_outage_start.erase(i);
    }
}

// crit loads and tdry are single year; pv, V, clipped are lifetime arrays
void resilience_runner::run_surviving_batteries_by_looping(double* crit_loads_kwac, double* pv_kwac, double* pv_kwdc,
                                                           double* V, double* pv_clipped_kw, double* tdry_c){
    size_t nrec = batt->step_per_year;
    size_t steps_lifetime = nrec * batt->nyears;
    size_t i = 0;
    while (get_n_surviving_batteries() > 0 && i < steps_lifetime){
        if (pv_kwdc && V && pv_clipped_kw && tdry_c)
            run_surviving_batteries(crit_loads_kwac[i % nrec], pv_kwac[i], pv_kwdc[i], V[i], pv_clipped_kw[i], tdry_c[i % nrec]);
        else
            run_surviving_batteries(crit_loads_kwac[i % nrec], pv_kwac[i]);
        i++;
    }

    if (battery_per_outage_start.empty())
        return;

    double total_load = std::accumulate(crit_loads_kwac, crit_loads_kwac + nrec, 0.0) * batt->nyears;
    for (auto& b : battery_per_outage_start){
        indices_survived[b.first] = steps_lifetime;
        total_load_met[b.first] = total_load;
    }
    battery_per_outage_start.clear();
}

// return average hours survived
double resilience_runner::compute_metrics(){
    outage_durations.clear();
    probs_of_surviving.clear();

    double hrs_total = (double)batt->step_per_hour * 8760. * (double)batt->nyears;
    outage_durations = std::vector<double>(indices_survived.begin(), indices_survived.end());;
    std::sort(outage_durations.begin(), outage_durations.end());
    outage_durations.erase(unique(outage_durations.begin(), outage_durations.end()), outage_durations.end());
    for (auto& i : outage_durations){
        double prob = std::count(indices_survived.begin(), indices_survived.end(), i) / hrs_total;
        i /= batt->step_per_hour;       // convert to hours
        probs_of_surviving.emplace_back(prob);
    }

    return std::accumulate(indices_survived.begin(), indices_survived.end(), 0.0)/batt->step_per_hour/(double)indices_survived.size();
}

size_t resilience_runner::get_n_surviving_batteries() {
    return battery_per_outage_start.size();
}

std::vector<double> resilience_runner::get_hours_survived() {
    double hours_per_step = 1. / batt->step_per_hour;
    std::vector<double> hours_survived;
    for (const auto& i : indices_survived)
        hours_survived.push_back(i * hours_per_step);
    return hours_survived;
}


double resilience_runner::get_avg_crit_load_kwh(){
    return std::accumulate(total_load_met.begin(), total_load_met.end(), 0.0) / (double)(total_load_met.size() * batt->step_per_hour);
}

std::vector<double> resilience_runner::get_outage_duration_hrs() {
    return outage_durations;
}

std::vector<double> resilience_runner::get_probs_of_surviving(){
    return probs_of_surviving;
}

std::vector<double> resilience_runner::get_cdf_of_surviving(){
    std::vector<double> cum_prob;
    cum_prob.push_back(probs_of_surviving[0]);
    for (size_t i = 1; i < probs_of_surviving.size(); i++){
        cum_prob.emplace_back(probs_of_surviving[i] + cum_prob[i-1]);
    }
    return cum_prob;
}

std::vector<double> resilience_runner::get_survival_function(){
    std::vector<double> survival_fx;
    survival_fx.push_back(1. - probs_of_surviving[0]);
    for (size_t i = 1; i < probs_of_surviving.size(); i++){
        survival_fx.emplace_back(survival_fx[i-1] - probs_of_surviving[i]);
    }
    if (survival_fx.back() < 1e-7)
        survival_fx.back() = 0.;
    return survival_fx;
}
