#include <JuceHeader.h>
#include <catch2/catch_session.hpp>

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    return Catch::Session().run (argc, argv);
}
