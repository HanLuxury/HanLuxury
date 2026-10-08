#include "ClothesRenderer.h"
#include "../character/CharacterBody.h"
#include "../character/CharacterHead.h"
#include "../character/CharacterHair.h"
namespace Eagle::Character {
ClothesRenderer::ClothesRenderer(std::shared_ptr<AssetTicket> ticket,Colour tint):ticket_(std::move(ticket)),tint_(tint) {}
bool ClothesRenderer::Initialise() {
    auto& rw=RenderWareApi::Get();
    if(!ticket_||!ticket_->asset||!ticket_->asset->clump) return false;
    clump_.reset(rw.cloneClump(ticket_->asset->clump.get()));
    std::vector<RpAtomic*> atomics;
    if(!clump_||!CollectAtomics(clump_.get(),atomics)||!skeleton_.Initialise(clump_.get(),atomics)) return false;
    // By geometry, not by the clone's frame name: NodeNameCopy keeps only 23 bytes of it.
    const auto& tags=ticket_->asset->parts;
    for(auto* atomic:atomics) {
        auto tag=tags.find(RpAtomicGetGeometry(atomic));if(tag==tags.end()) return false;
        parts_.push_back({atomic,tag->second.region,tag->second.material});
    }return true;
}
bool ClothesRenderer::Prepare(RpClump* carrier,const RwMatrix& root) {
    if(!FiniteMatrix(root)||!skeleton_.Update(carrier)) return false;
    auto* frame=RpClumpGetFrame(clump_.get());if(!frame) return false;
    frame->modelling=root;RwFrameUpdateObjects(frame);return true;
}
void ClothesRenderer::Render(uint16_t hidden,Colour skin,Colour hair,RwTexture* face,RwTexture* body) {
    for(const auto& p:parts_) {
        if(CharacterBody::IsHidden(p.region,hidden)) continue;
        const bool isFace=CharacterHead::IsMaterial(p.material),isSkin=p.material=="eg_skin"||p.material.rfind("eg_skin_",0)==0;
        CharacterMaterial scoped(RpAtomicGetGeometry(p.atomic),(isSkin||isFace) ? skin:(CharacterHair::IsMaterial(p.material) ? hair:tint_),isFace ? face:(isSkin ? body:nullptr));
        AtomicDefaultRenderCallBack(p.atomic);
    }
}
}
