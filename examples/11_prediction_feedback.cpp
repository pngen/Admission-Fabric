#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{6} << 30);
    // Observe real (high) peak VRAM for a model, feedback should raise confidence.
    for (int i = 0; i < 4; ++i) {
        Observation o;
        o.id = ObservationId(i + 1);
        o.model_id = ModelId(7); o.model_revision = ModelRevision(1);
        o.backend = "cuda"; o.dtype = "fp16"; o.quantization = "none";
        o.peak_vram = ByteCount{2} << 30;
        o.input_tokens = 1000; o.output_tokens = 1000;
        o.execution_duration = Milliseconds(200);
        o.completed_at = SystemClock::now();
        f.observe(o);
    }
    auto pred = f.predictor().predict_memory(afex::desc(AdmissionRequestId(1), 1000, 1000));
    std::printf("EXAMPLE prediction-feedback: confidence=%.3f evidence=%u\n", pred.confidence, pred.evidence_count);
    return pred.confidence > 0.5 ? 0 : 1;
}
