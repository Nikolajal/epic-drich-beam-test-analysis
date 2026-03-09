#pragma once
#include <string>

/**
 * @todo: Make a single file with (class?) writers/readers
 * @tood: Generate a class for the standard analysis to have method to recover standard flow results: n photons, sigma, etc
 */

void standard_analysis(
    std::string data_repository,
    std::string run_name,
    int max_spill);

class standard_analysis
{
private:
protected:
public:
};