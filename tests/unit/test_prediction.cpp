#include "test_util.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/descriptor.hpp"
using namespace admission_fabric;
static AdmissionDescriptor d10() {
    AdmissionDescriptor d;
    d.model_id=ModelId(7); d.model_revision=ModelRevision(1); d.backend="cuda"; d.dtype="fp16";
    d.input_tokens=1000; d.expected_output_tokens=1000; d.max_output_tokens=1000;
    return d;
}
AF_TEST(prediction_cold_start_low_confidence) {
    WorkloadPredictor p;
    auto mem = p.predict_memory(d10());
    AF_CHECK(mem.fallback);
    AF_CHECK(mem.confidence <= 0.35);
    AF_CHECK(mem.footprint.kv_growth > 0);
}
AF_TEST(prediction_updates_with_observation) {
    WorkloadPredictor p;
    PredictionProfile pr; pr.model_id=ModelId(7); pr.revision=ModelRevision(1); pr.backend="cuda"; pr.dtype="fp16";
    pr.fixed_base_memory=ByteCount{1}<<30; pr.per_token_kv_bytes=1024; pr.per_token_tensor_bytes=1024;
    pr.prefill_tokens_per_sec=1000; pr.decode_tokens_per_sec=50;
    AF_CHECK(p.register_profile(pr));
    auto before = p.predict_memory(d10());
    AF_CHECK(!before.fallback);
    AF_CHECK(before.confidence > 0.5);
    Observation o;
    o.id=ObservationId(1); o.admission_request_id=AdmissionRequestId(1); o.model_id=ModelId(7); o.model_revision=ModelRevision(1);
    o.backend="cuda"; o.dtype="fp16"; o.quantization="none";
    o.peak_vram=ByteCount{2}<<30; o.input_tokens=1000; o.output_tokens=1000; o.execution_duration=Milliseconds(200);
    o.completed_at=SystemClock::now(); o.kind=ObservationKind::Completed;
    p.observe(o);
    AF_CHECK(p.evidence_for_model(ModelId(7)) >= 1);
    auto after = p.predict_memory(d10());
    AF_CHECK(after.evidence_count >= 1);
}
AF_TEST_MAIN("admission-fabric-prediction");
