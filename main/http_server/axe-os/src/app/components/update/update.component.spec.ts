import { ComponentFixture, TestBed, fakeAsync, tick } from '@angular/core/testing';
import { HttpResponse, provideHttpClient } from '@angular/common/http';
import { BehaviorSubject, of } from 'rxjs';

import { UpdateComponent } from './update.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { CheckboxModule } from 'primeng/checkbox';
import { ProgressBarModule } from 'primeng/progressbar';
import { ButtonModule } from 'primeng/button';
import { provideToastr, ToastrService } from 'ngx-toastr';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import {
  BridgeInfo, BridgeUpdateStatus, SystemInfo
} from 'src/app/generated/models';

describe('UpdateComponent', () => {
  let component: UpdateComponent;
  let fixture: ComponentFixture<UpdateComponent>;
  let info: BehaviorSubject<SystemInfo>;
  let systemService: jasmine.SpyObj<SystemApiService>;

  const bridgeInfo: BridgeInfo = {
    available: true,
    versionQuerySupported: true,
    version: '1.2.3+gabcdef',
    protocolMajor: 1,
    protocolMinor: 0,
  };

  beforeEach(() => {
    info = new BehaviorSubject({ ASICModel: 'BZM' } as SystemInfo);
    systemService = jasmine.createSpyObj<SystemApiService>(
      'SystemApiService', [
        'getBridgeInfo',
        'performBridgeUpdate',
        'getBridgeFirmwareUpdateStatus',
        'performOTAUpdate',
        'performWWWOTAUpdate',
      ]);
    systemService.getBridgeInfo.and.returnValue(of(bridgeInfo));

    TestBed.configureTestingModule({
      declarations: [UpdateComponent, ModalComponent],
      imports: [
        FileUploadModule, CheckboxModule, ButtonModule, ProgressBarModule
      ],
      providers: [
        provideHttpClient(),
        provideToastr(),
        { provide: LiveDataService, useValue: { info$: info.asObservable() } },
        { provide: SystemApiService, useValue: systemService },
      ]
    });
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('shows bridge version and uploader for BZM products', () => {
    fixture.detectChanges();
    const text = fixture.nativeElement.textContent;
    expect(text).toContain('Update Bridge Firmware');
    expect(text).toContain('Current Version: 1.2.3+gabcdef');
    expect(systemService.getBridgeInfo).toHaveBeenCalled();
  });

  it('does not expose bridge update controls on non-BZM products', () => {
    info.next({ ASICModel: 'BM1370' } as SystemInfo);
    fixture.detectChanges();
    expect(fixture.nativeElement.textContent)
      .not.toContain('Update Bridge Firmware');
  });

  it('rejects non-bin bridge uploads before making a request', () => {
    const toastr = TestBed.inject(ToastrService);
    spyOn(toastr, 'error');
    const file = new File(['invalid'], 'bridge.uf2');

    component.bridgeUpdate({ files: [file] } as any);

    expect(toastr.error).toHaveBeenCalled();
    expect(systemService.performBridgeUpdate).not.toHaveBeenCalled();
  });

  it('polls an accepted bridge update through version confirmation',
     fakeAsync(() => {
    const running: BridgeUpdateStatus = {
      state: 'preparing',
      progress: 0,
      imageSize: 1024,
      running: true,
      manifestValidated: true,
      forceRequested: false,
      targetBoardVersion: 1002,
      imageVersion: '1.2.4+g1234567',
      imageProtocolMajor: 1,
      imageProtocolMinor: 0,
      versionQuerySupported: false,
      currentVersion: null,
      error: null,
    };
    const complete: BridgeUpdateStatus = {
      ...running,
      state: 'complete',
      progress: 100,
      running: false,
      versionQuerySupported: true,
      currentVersion: '1.2.4+g1234567',
    };
    systemService.performBridgeUpdate.and.returnValue(of(
      new HttpResponse({ status: 202, body: running })));
    systemService.getBridgeFirmwareUpdateStatus.and.returnValue(of(complete));

    component.bridgeUpdate({
      files: [new File(['firmware'], 'bonanza-bridge-fw.bin')]
    } as any);
    tick(0);

    expect(systemService.getBridgeFirmwareUpdateStatus).toHaveBeenCalled();
    expect(component.updateStatus).toBe('success');
    expect(component.updateMessage).toContain('1.2.4+g1234567');
  }));
});
