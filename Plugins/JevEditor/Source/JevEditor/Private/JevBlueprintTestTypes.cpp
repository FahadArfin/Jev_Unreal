#include "JevBlueprintTestTypes.h"
#include "Kismet2/CompilerResultsLog.h"

void UJevBlueprintErrorFixtureNode::EarlyValidation(FCompilerResultsLog& MessageLog) const
{
    Super::EarlyValidation(MessageLog);
#if WITH_DEV_AUTOMATION_TESTS
    if (GIsAutomationTesting) MessageLog.Error(TEXT("Jev deliberate compile diagnostic fixture @@"), this);
#endif
}
