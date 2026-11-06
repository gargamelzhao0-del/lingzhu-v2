"""Exercise the real catalog/inventory with temporary files, never a physical SD.

Only the explicit NO_MEM case substitutes a catalog-load result. CRC failures
come from corrupt dual-slot manifests; disappearance removes a real manifest.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-verification-inventory'
out.mkdir(parents=True, exist_ok=True)
catalog = (root / 'main/sd_audio_catalog.c').read_text(encoding='utf-8-sig')
catalog = catalog.replace('#include "sd_audio_verify_catalog.inc"', r'''
static bool inject_load_nomem;
static esp_err_t inventory_load(const char *id, sd_audio_session_t *scene) {
    if (inject_load_nomem) { inject_load_nomem=false; return ESP_ERR_NO_MEM; }
    return sd_audio_catalog_load(id,scene);
}
#define sd_audio_catalog_load inventory_load
#include "sd_audio_verify_catalog.inc"
#undef sd_audio_catalog_load
''')
code = '#include <assert.h>\n' + catalog + r'''
static sd_audio_session_t fixture(const char *id) {
    sd_audio_session_t scene={.complete=1U,.segment_count=1U,.boot_id=7U,.start_ms=1000U};
    strcpy(scene.session_id,id);memset(scene.owner,0x31,32U);
    assert(sd_audio_catalog_create(&scene)==ESP_OK);
    sd_audio_segment_t part={.sealed=1U,.total_bytes=794U,.payload_crc=12345U};
    snprintf(part.path,sizeof(part.path),"./card_%s_1.ready",id);
    assert(sd_audio_catalog_write(id,&part)==ESP_OK);
    return scene;
}
static void corrupt(const char *id) {
    char path[128];assert(catalog_path(path,id,"session"));
    FILE *file=fopen(path,"r+b");assert(file);
    uint8_t bytes[2*sizeof(sd_audio_session_t)];memset(bytes,0xa5,sizeof(bytes));
    assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes)&&!fclose(file));
}
static void checkpoint(const sd_audio_session_t *scene,const sd_audio_segment_t *part) {
    sd_audio_lease_token_t lease=0U;
    assert(sd_audio_catalog_lease_acquire(scene,part,SD_AUDIO_LEASE_VERIFY,3U,1U,&lease)==ESP_OK);
    assert(sd_audio_catalog_verify_checkpoint(scene,99U,3U,part->index+1U,lease)==ESP_OK);
    assert(sd_audio_catalog_lease_release(lease,SD_AUDIO_LEASE_VERIFY)==ESP_OK);
}
int main(int argc,char **argv) {
    assert(argc==2&&sd_audio_catalog_init()==ESP_OK);
    const sd_audio_session_t original=fixture("scene_a");
    sd_audio_session_t scene;sd_audio_segment_t part;
    if(!strcmp(argv[1],"crc")) {
        corrupt(original.session_id);
        assert(sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part)==ESP_ERR_INVALID_CRC);
        puts("PASS real unreadable manifest reports CRC failure instead of successful inventory exhaustion");
    } else if(!strcmp(argv[1],"nomem")) {
        inject_load_nomem=true;
        assert(sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part)==ESP_ERR_NO_MEM);
        sd_audio_catalog_verify_reset();
        assert(sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part)==ESP_OK);
        puts("PASS explicit resource pressure defers inventory without recording a false completed scan");
    } else if(!strcmp(argv[1],"missing")) {
        (void)fixture("scene_b");
        assert(sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part)==ESP_OK);
        char removed[48],path[128];strcpy(removed,scene.session_id);
        assert(catalog_path(path,removed,"session")&&!unlink(path));
        assert(sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part)==ESP_OK);
        assert(strcmp(removed,scene.session_id));
        puts("PASS disappearing resumed scene does not turn remaining scenes into global end of inventory");
    } else {
        assert(!strcmp(argv[1],"progress"));
        (void)fixture("scene_b");corrupt(original.session_id);
        unsigned verified=0U;esp_err_t err=ESP_OK;
        for(unsigned step=0U;step<100U;++step) {
            err=sd_audio_catalog_verify_next(original.owner,99U,3U,&scene,&part);
            if(err==ESP_ERR_NOT_FINISHED)continue;
            if(err!=ESP_OK)break;
            assert(!strcmp(scene.session_id,"scene_b"));checkpoint(&scene,&part);++verified;
        }
        assert(verified==1U&&err==ESP_ERR_INVALID_CRC);
        assert(sd_audio_catalog_read("scene_b",0U,&part)==ESP_OK&&!part.acknowledged&&!part.missing);
        puts("PASS unreadable scene does not starve a healthy scene; pass retains its error and never creates ACK/missing");
    }
    sd_audio_catalog_verify_reset();return 0;
}
'''
source = out / 'inventory.c'
source.write_text(code, encoding='utf-8')
exe = out / 'inventory.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109',
    '-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'), '-I'+str(root/'main'),
    str(source), '-o', str(exe)], check=True)
failed=[]
for name in ('crc', 'nomem', 'missing', 'progress'):
    with tempfile.TemporaryDirectory(prefix='sd-inventory-'+name+'-') as directory:
        run=subprocess.run([str(exe),name],cwd=directory,text=True,capture_output=True,timeout=30)
    print(name+': '+run.stdout+run.stderr,flush=True)
    if run.returncode: failed.append(name)
if failed: raise RuntimeError('inventory regression failures: '+', '.join(failed))
