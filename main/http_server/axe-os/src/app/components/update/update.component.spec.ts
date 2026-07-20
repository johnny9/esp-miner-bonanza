import { ComponentFixture, TestBed } from '@angular/core/testing';
import { provideHttpClient } from '@angular/common/http';
import { BehaviorSubject } from 'rxjs';

import { UpdateComponent } from './update.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { CheckboxModule } from 'primeng/checkbox';
import { ProgressBarModule } from 'primeng/progressbar';
import { ButtonModule } from 'primeng/button';
import { provideToastr } from 'ngx-toastr';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { SystemInfo } from 'src/app/generated/models';

describe('UpdateComponent', () => {
  let component: UpdateComponent;
  let fixture: ComponentFixture<UpdateComponent>;

  beforeEach(() => {
    const info = new BehaviorSubject({ ASICModel: 'BZM' } as SystemInfo);
    const systemService = jasmine.createSpyObj<SystemApiService>(
      'SystemApiService', ['performOTAUpdate', 'performWWWOTAUpdate']);

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

  it('shows only the supported ESP and AxeOS production update targets', () => {
    expect(component).toBeTruthy();
    const text = fixture.nativeElement.textContent;
    expect(text).toContain('Update Firmware');
    expect(text).toContain('Update AxeOS');
    expect(text).not.toContain('Update Bridge Firmware');
  });
});
